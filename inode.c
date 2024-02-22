#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "bitmap.h"
#include "simplefs.h"

static const struct inode_operations simplefs_inode_ops;
static const struct inode_operations symlink_inode_ops;

/* Get inode ino from disk */
struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
    struct inode *inode = NULL;
    struct simplefs_inode *cinode = NULL;
    struct simplefs_inode_info *ci = NULL;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh = NULL;
    uint32_t inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;
    uint32_t inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;
    int ret;

    /* Fail if ino is out of range */
    if (ino >= sbi->nr_inodes)
        return ERR_PTR(-EINVAL);

    /* Get a locked inode from Linux */
    inode = iget_locked(sb, ino);
    if (!inode)
        return ERR_PTR(-ENOMEM);

    /* If inode is in cache, return it */
    if (!(inode->i_state & I_NEW))
        return inode;

    ci = SIMPLEFS_INODE(inode);
    /* Read inode from disk and initialize */
    bh = sb_bread(sb, inode_block);
    if (!bh) {
        ret = -EIO;
        goto failed;
    }

    cinode = (struct simplefs_inode *) bh->b_data;
    cinode += inode_shift;

    inode->i_ino = ino;
    inode->i_sb = sb;
    inode->i_op = &simplefs_inode_ops;

    inode->i_mode = le32_to_cpu(cinode->i_mode);
    i_uid_write(inode, le32_to_cpu(cinode->i_uid));
    i_gid_write(inode, le32_to_cpu(cinode->i_gid));
    inode->i_size = le32_to_cpu(cinode->i_size);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    inode_set_ctime(inode, (time64_t) le32_to_cpu(cinode->i_ctime), 0);
#else
    inode->i_ctime.tv_sec = (time64_t) le32_to_cpu(cinode->i_ctime);
    inode->i_ctime.tv_nsec = 0;
#endif

    inode->i_atime.tv_sec = (time64_t) le32_to_cpu(cinode->i_atime);
    inode->i_atime.tv_nsec = 0;
    inode->i_mtime.tv_sec = (time64_t) le32_to_cpu(cinode->i_mtime);
    inode->i_mtime.tv_nsec = 0;
    inode->i_blocks = le32_to_cpu(cinode->i_blocks);
    set_nlink(inode, le32_to_cpu(cinode->i_nlink));

    if (S_ISDIR(inode->i_mode)) {
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        inode->i_fop = &simplefs_dir_ops;
    } else if (S_ISREG(inode->i_mode)) {
        ci->ei_block = le32_to_cpu(cinode->ei_block);
        inode->i_fop = &simplefs_file_ops;
        inode->i_mapping->a_ops = &simplefs_aops;
    } else if (S_ISLNK(inode->i_mode)) {
        strncpy(ci->i_data, cinode->i_data, sizeof(ci->i_data));
        inode->i_link = ci->i_data;
        inode->i_op = &symlink_inode_ops;
    }

    brelse(bh);

    /* Unlock the inode to make it usable */
    unlock_new_inode(inode);

    return inode;

failed:
    brelse(bh);
    iget_failed(inode);
    return ERR_PTR(ret);
}

/* Searches for a dentry in dir.
 * Fills dentry with NULL if not found in dir, or with the corresponding inode
 * if found.
 * Returns NULL on success, indicating the dentry was successfully filled or
 * confirmed absent.
 */
static struct dentry *simplefs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct inode *inode = NULL;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock = NULL;
    struct simplefs_file *f = NULL;
    int ei, bi, fi;

    /* Check filename length */
    if (dentry->d_name.len > SIMPLEFS_FILENAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    /* Read the directory block on disk */
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return ERR_PTR(-EIO);
    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    /* Search for the file in directory */
    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        if (!eblock->extents[ei].ee_start)
            break;

        /* Iterate blocks in extent */
        for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            if (!bh2)
                return ERR_PTR(-EIO);

            dblock = (struct simplefs_dir_block *) bh2->b_data;

            /* Search file in ei_block */
            for (fi = 0; fi < SIMPLEFS_FILES_PER_BLOCK; fi++) {
                f = &dblock->files[fi];
                if (!f->inode) {
                    brelse(bh2);
                    goto search_end;
                }
                if (!strncmp(f->filename, dentry->d_name.name,
                             SIMPLEFS_FILENAME_LEN)) {
                    inode = simplefs_iget(sb, f->inode);
                    brelse(bh2);
                    goto search_end;
                }
            }
            brelse(bh2);
            bh2 = NULL;
        }
    }

search_end:
    brelse(bh);

    /* Update directory access time */
    dir->i_atime = current_time(dir);
    mark_inode_dirty(dir);

    /* Fill the dentry with the inode */
    d_add(dentry, inode);

    return NULL;
}

/* Create a new inode in dir */
static struct inode *simplefs_new_inode(struct inode *dir, mode_t mode)
{
    struct inode *inode;
    struct simplefs_inode_info *ci;
    struct super_block *sb;
    struct simplefs_sb_info *sbi;
    uint32_t ino, bno;
    int ret;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    struct timespec64 cur_time;
#endif

    /* Check mode before doing anything to avoid undoing everything */
    if (!S_ISDIR(mode) && !S_ISREG(mode) && !S_ISLNK(mode)) {
        pr_err(
            "File type not supported (only directory, regular file and symlink "
            "supported)\n");
        return ERR_PTR(-EINVAL);
    }

    /* Check if inodes are available */
    sb = dir->i_sb;
    sbi = SIMPLEFS_SB(sb);
    if (sbi->nr_free_inodes == 0 || sbi->nr_free_blocks == 0)
        return ERR_PTR(-ENOSPC);

    /* Get a new free inode */
    ino = get_free_inode(sbi);
    if (!ino)
        return ERR_PTR(-ENOSPC);

    inode = simplefs_iget(sb, ino);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto put_ino;
    }

    if (S_ISLNK(mode)) {
#if MNT_IDMAP_REQUIRED()
        inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
#elif USER_NS_REQUIRED()
        inode_init_owner(&init_user_ns, inode, dir, mode);
#else
        inode_init_owner(inode, dir, mode);
#endif
        set_nlink(inode, 1);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
        cur_time = current_time(inode);
        inode->i_atime = inode->i_mtime = cur_time;
        inode_set_ctime_to_ts(inode, cur_time);
#else
        inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
#endif
        inode->i_op = &symlink_inode_ops;
        return inode;
    }

    ci = SIMPLEFS_INODE(inode);

    /* Get a free block for this new inode's index */
    bno = get_free_blocks(sbi, 1);
    if (!bno) {
        ret = -ENOSPC;
        goto put_inode;
    }

    /* Initialize inode */
#if MNT_IDMAP_REQUIRED()
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
#elif USER_NS_REQUIRED()
    inode_init_owner(&init_user_ns, inode, dir, mode);
#else
    inode_init_owner(inode, dir, mode);
#endif
    inode->i_blocks = 1;
    if (S_ISDIR(mode)) {
        ci->ei_block = bno;
        inode->i_size = SIMPLEFS_BLOCK_SIZE;
        inode->i_fop = &simplefs_dir_ops;
        set_nlink(inode, 2); /* . and .. */
    } else if (S_ISREG(mode)) {
        ci->ei_block = bno;
        inode->i_size = 0;
        inode->i_fop = &simplefs_file_ops;
        inode->i_mapping->a_ops = &simplefs_aops;
        set_nlink(inode, 1);
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    cur_time = current_time(inode);
    inode->i_atime = inode->i_mtime = cur_time;
    inode_set_ctime_to_ts(inode, cur_time);
#else
    inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
#endif

    return inode;

put_inode:
    iput(inode);
put_ino:
    put_inode(sbi, ino);

    return ERR_PTR(ret);
}

/* Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
#if MNT_IDMAP_REQUIRED()
static int simplefs_create(struct mnt_idmap *id,
                           struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
#elif USER_NS_REQUIRED()
static int simplefs_create(struct user_namespace *ns,
                           struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
#else
static int simplefs_create(struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
#endif
{
    struct super_block *sb;
    struct inode *inode;
    struct simplefs_inode_info *ci_dir;
    struct simplefs_file_ei_block *eblock;
    struct simplefs_dir_block *dblock;
    char *fblock;
    struct buffer_head *bh, *bh2;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    struct timespec64 cur_time;
#endif

    int ret = 0, alloc = false, bno = 0;
    int ei = 0, bi = 0, fi = 0;

    /* Check filename length */
    if (strlen(dentry->d_name.name) > SIMPLEFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Read parent directory index */
    ci_dir = SIMPLEFS_INODE(dir);
    sb = dir->i_sb;
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    /* Check if parent directory is full */
    if (eblock->nr_files == SIMPLEFS_MAX_SUBFILES) {
        ret = -EMLINK;
        goto end;
    }

    /* Get a new free inode */
    inode = simplefs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto end;
    }

    /* Scrub ei_block for new file/directory to avoid previous data
     * messing with new file/directory.
     */
    bh2 = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    if (!bh2) {
        ret = -EIO;
        goto iput;
    }
    fblock = (char *) bh2->b_data;
    memset(fblock, 0, SIMPLEFS_BLOCK_SIZE);
    mark_buffer_dirty(bh2);
    brelse(bh2);

    /* Find first free slot in parent index and register new inode */
    ei = eblock->nr_files / SIMPLEFS_FILES_PER_EXT;
    bi = eblock->nr_files % SIMPLEFS_FILES_PER_EXT / SIMPLEFS_FILES_PER_BLOCK;
    fi = eblock->nr_files % SIMPLEFS_FILES_PER_BLOCK;

    if (!eblock->extents[ei].ee_start) {
        bno = get_free_blocks(SIMPLEFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto iput;
        }
        eblock->extents[ei].ee_start = bno;
        eblock->extents[ei].ee_len = 8;
        eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block +
                                                eblock->extents[ei - 1].ee_len
                                          : 0;
        alloc = true;
    }
    bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
    if (!bh2) {
        ret = -EIO;
        goto put_block;
    }
    dblock = (struct simplefs_dir_block *) bh2->b_data;

    dblock->files[fi].inode = inode->i_ino;
    strncpy(dblock->files[fi].filename, dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);

    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    /* Update stats and mark dir and new inode dirty */
    mark_inode_dirty(inode);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    cur_time = current_time(dir);
    dir->i_mtime = dir->i_atime = cur_time;
    inode_set_ctime_to_ts(dir, cur_time);
#else
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
#endif

    if (S_ISDIR(mode))
        inc_nlink(dir);
    mark_inode_dirty(dir);

    /* setup dentry */
    d_instantiate(dentry, inode);

    return 0;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct simplefs_extent));
    }
iput:
    put_blocks(SIMPLEFS_SB(sb), SIMPLEFS_INODE(inode)->ei_block, 1);
    put_inode(SIMPLEFS_SB(sb), inode->i_ino);
    iput(inode);
end:
    brelse(bh);
    return ret;
}

static int simplefs_remove_from_dir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh = NULL, *bh2 = NULL, *bh_prev = NULL;
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock = NULL, *dblock_prev = NULL;
    int ei = 0, bi = 0, fi = 0;
    int ret = 0, found = false;

    /* Read parent directory index */
    bh = sb_bread(sb, SIMPLEFS_INODE(dir)->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        if (!eblock->extents[ei].ee_start)
            break;

        for (bi = 0; bi < eblock->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            if (!bh2) {
                ret = -EIO;
                goto release_bh;
            }
            dblock = (struct simplefs_dir_block *) bh2->b_data;
            if (!dblock->files[0].inode)
                break;

            if (found) {
                memmove(dblock_prev->files + SIMPLEFS_FILES_PER_BLOCK - 1,
                        dblock->files, sizeof(struct simplefs_file));
                brelse(bh_prev);

                memmove(dblock->files, dblock->files + 1,
                        (SIMPLEFS_FILES_PER_BLOCK - 1) *
                            sizeof(struct simplefs_file));
                memset(dblock->files + SIMPLEFS_FILES_PER_BLOCK - 1, 0,
                       sizeof(struct simplefs_file));
                mark_buffer_dirty(bh2);

                bh_prev = bh2;
                dblock_prev = dblock;
                continue;
            }
            /* Remove file from parent directory */
            for (fi = 0; fi < SIMPLEFS_FILES_PER_BLOCK; fi++) {
                if (dblock->files[fi].inode == inode->i_ino &&
                    !strcmp(dblock->files[fi].filename, dentry->d_name.name)) {
                    found = true;
                    if (fi != SIMPLEFS_FILES_PER_BLOCK - 1) {
                        memmove(dblock->files + fi, dblock->files + fi + 1,
                                (SIMPLEFS_FILES_PER_BLOCK - fi - 1) *
                                    sizeof(struct simplefs_file));
                    }
                    memset(dblock->files + SIMPLEFS_FILES_PER_BLOCK - 1, 0,
                           sizeof(struct simplefs_file));
                    mark_buffer_dirty(bh2);
                    bh_prev = bh2;
                    dblock_prev = dblock;
                    break;
                }
            }
            if (!found)
                brelse(bh2);
        }
    }
    if (found) {
        if (bh_prev)
            brelse(bh_prev);
        eblock->nr_files--;
        mark_buffer_dirty(bh);
    }
release_bh:
    brelse(bh);
    return ret;
}

/* Remove a link for a file including the reference in the parent directory.
 * If link count is 0, destroy file in this way:
 *   - remove the file from its parent directory.
 *   - cleanup blocks containing data
 *   - cleanup file index block
 *   - cleanup inode
 */
static int simplefs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct simplefs_file_ei_block *file_block = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    struct timespec64 cur_time;
#endif
    int ei = 0, bi = 0;
    int ret = 0;

    uint32_t ino = inode->i_ino;
    uint32_t bno = 0;

    ret = simplefs_remove_from_dir(dir, dentry);
    if (ret != 0)
        return ret;

    if (S_ISLNK(inode->i_mode))
        goto clean_inode;

        /* Update inode stats */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    cur_time = current_time(dir);
    dir->i_mtime = dir->i_atime = cur_time;
    inode_set_ctime_to_ts(dir, cur_time);
#else
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
#endif

    if (S_ISDIR(inode->i_mode)) {
        drop_nlink(dir);
        drop_nlink(inode);
    }
    mark_inode_dirty(dir);

    if (inode->i_nlink > 1) {
        inode_dec_link_count(inode);
        return ret;
    }

    /* Cleans up pointed blocks when unlinking a file. If reading the index
     * block fails, the inode is cleaned up regardless, resulting in the
     * permanent loss of this file's blocks. If scrubbing a data block fails,
     * do not terminate the operation (as it is already too late); instead,
     * release the block and proceed.
     */
    bno = SIMPLEFS_INODE(inode)->ei_block;
    bh = sb_bread(sb, bno);
    if (!bh)
        goto clean_inode;
    file_block = (struct simplefs_file_ei_block *) bh->b_data;
    if (S_ISDIR(inode->i_mode))
        goto scrub;

    for (ei = 0; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        char *block;

        if (!file_block->extents[ei].ee_start)
            break;

        put_blocks(sbi, file_block->extents[ei].ee_start,
                   file_block->extents[ei].ee_len);

        /* Scrub the extent */
        for (bi = 0; bi < file_block->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, file_block->extents[ei].ee_start + bi);
            if (!bh2)
                continue;
            block = (char *) bh2->b_data;
            memset(block, 0, SIMPLEFS_BLOCK_SIZE);
            mark_buffer_dirty(bh2);
            brelse(bh2);
        }
    }

scrub:
    /* Scrub index block */
    memset(file_block, 0, SIMPLEFS_BLOCK_SIZE);
    mark_buffer_dirty(bh);
    brelse(bh);

clean_inode:
    /* Cleanup inode and mark dirty */
    inode->i_blocks = 0;
    SIMPLEFS_INODE(inode)->ei_block = 0;
    inode->i_size = 0;
    i_uid_write(inode, 0);
    i_gid_write(inode, 0);
    inode->i_mode = 0;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
    inode_set_ctime(inode, 0, 0);
#else
    inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
#endif

    drop_nlink(inode);
    mark_inode_dirty(inode);

    /* Free inode and index block from bitmap */
    put_blocks(sbi, bno, 1);
    put_inode(sbi, ino);

    return ret;
}

#if MNT_IDMAP_REQUIRED()
static int simplefs_rename(struct mnt_idmap *id,
                           struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
#elif USER_NS_REQUIRED()
static int simplefs_rename(struct user_namespace *ns,
                           struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
#else
static int simplefs_rename(struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
#endif
{
    struct super_block *sb = old_dir->i_sb;
    struct simplefs_inode_info *ci_new = SIMPLEFS_INODE(new_dir);
    struct inode *src = d_inode(old_dentry);
    struct buffer_head *bh_new = NULL, *bh2 = NULL;
    struct simplefs_file_ei_block *eblock_new = NULL;
    struct simplefs_dir_block *dblock = NULL;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    struct timespec64 cur_time;
#endif

    int new_pos = -1, ret = 0;
    int ei = 0, bi = 0, fi = 0, bno = 0;

    /* fail with these unsupported flags */
    if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;

    /* Check if filename is not too long */
    if (strlen(new_dentry->d_name.name) > SIMPLEFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Fail if new_dentry exists or if new_dir is full */
    bh_new = sb_bread(sb, ci_new->ei_block);
    if (!bh_new)
        return -EIO;

    eblock_new = (struct simplefs_file_ei_block *) bh_new->b_data;
    for (ei = 0; new_pos < 0 && ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        if (!eblock_new->extents[ei].ee_start)
            break;

        for (bi = 0; new_pos < 0 && bi < eblock_new->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock_new->extents[ei].ee_start + bi);
            if (!bh2) {
                ret = -EIO;
                goto release_new;
            }

            dblock = (struct simplefs_dir_block *) bh2->b_data;
            for (fi = 0; fi < SIMPLEFS_FILES_PER_BLOCK; fi++) {
                if (new_dir == old_dir) {
                    if (!strncmp(dblock->files[fi].filename,
                                 old_dentry->d_name.name,
                                 SIMPLEFS_FILENAME_LEN)) {
                        strncpy(dblock->files[fi].filename,
                                new_dentry->d_name.name, SIMPLEFS_FILENAME_LEN);
                        mark_buffer_dirty(bh2);
                        brelse(bh2);
                        goto release_new;
                    }
                }
                if (!strncmp(dblock->files[fi].filename,
                             new_dentry->d_name.name, SIMPLEFS_FILENAME_LEN)) {
                    brelse(bh2);
                    ret = -EEXIST;
                    goto release_new;
                }
                if (new_pos < 0 && !dblock->files[fi].inode) {
                    new_pos = fi;
                    break;
                }
            }
            if (new_pos < 0)
                brelse(bh2);
        }
    }

    /* If new directory is full, fail */
    if (new_pos < 0 && eblock_new->nr_files == SIMPLEFS_FILES_PER_EXT) {
        ret = -EMLINK;
        goto release_new;
    }

    /* insert in new parent directory */
    /* Get new freeblocks for extent if needed*/
    if (new_pos < 0) {
        bno = get_free_blocks(SIMPLEFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto release_new;
        }
        eblock_new->extents[ei].ee_start = bno;
        eblock_new->extents[ei].ee_len = 8;
        eblock_new->extents[ei].ee_block =
            ei ? eblock_new->extents[ei - 1].ee_block +
                     eblock_new->extents[ei - 1].ee_len
               : 0;
        bh2 = sb_bread(sb, eblock_new->extents[ei].ee_start + 0);
        if (!bh2) {
            ret = -EIO;
            goto put_block;
        }
        dblock = (struct simplefs_dir_block *) bh2->b_data;
        mark_buffer_dirty(bh_new);
        new_pos = 0;
    }
    dblock->files[new_pos].inode = src->i_ino;
    strncpy(dblock->files[new_pos].filename, new_dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);
    mark_buffer_dirty(bh2);
    brelse(bh2);

    /* Update new parent inode metadata */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    cur_time = current_time(new_dir);
    new_dir->i_atime = new_dir->i_mtime = cur_time;
    inode_set_ctime_to_ts(new_dir, cur_time);
#else
    new_dir->i_atime = new_dir->i_ctime = new_dir->i_mtime =
        current_time(new_dir);
#endif

    if (S_ISDIR(src->i_mode))
        inc_nlink(new_dir);
    mark_inode_dirty(new_dir);

    /* remove target from old parent directory */
    ret = simplefs_remove_from_dir(old_dir, old_dentry);
    if (ret != 0)
        goto release_new;

        /* Update old parent inode metadata */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    cur_time = current_time(old_dir);
    old_dir->i_atime = old_dir->i_mtime = cur_time;
    inode_set_ctime_to_ts(old_dir, cur_time);
#else
    old_dir->i_atime = old_dir->i_ctime = old_dir->i_mtime =
        current_time(old_dir);
#endif

    if (S_ISDIR(src->i_mode))
        drop_nlink(old_dir);
    mark_inode_dirty(old_dir);

    return ret;

put_block:
    if (eblock_new->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock_new->extents[ei].ee_start,
                   eblock_new->extents[ei].ee_len);
        memset(&eblock_new->extents[ei], 0, sizeof(struct simplefs_extent));
    }
release_new:
    brelse(bh_new);
    return ret;
}

#if MNT_IDMAP_REQUIRED()
static int simplefs_mkdir(struct mnt_idmap *id,
                          struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(id, dir, dentry, mode | S_IFDIR, 0);
}
#elif USER_NS_REQUIRED()
static int simplefs_mkdir(struct user_namespace *ns,
                          struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(ns, dir, dentry, mode | S_IFDIR, 0);
}
#else
static int simplefs_mkdir(struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(dir, dentry, mode | S_IFDIR, 0);
}
#endif

static int simplefs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh;
    struct simplefs_file_ei_block *eblock;

    /* If the directory is not empty, fail */
    if (inode->i_nlink > 2)
        return -ENOTEMPTY;

    bh = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    if (eblock->nr_files != 0) {
        brelse(bh);
        return -ENOTEMPTY;
    }
    brelse(bh);

    /* Remove directory with unlink */
    return simplefs_unlink(dir, dentry);
}

static int simplefs_link(struct dentry *old_dentry,
                         struct inode *dir,
                         struct dentry *dentry)
{
    struct inode *inode = d_inode(old_dentry);
    struct super_block *sb = inode->i_sb;
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    int ret = 0, alloc = false, bno = 0;
    int ei = 0, bi = 0, fi = 0;

    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;

    eblock = (struct simplefs_file_ei_block *) bh->b_data;
    if (eblock->nr_files == SIMPLEFS_MAX_SUBFILES) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    ei = eblock->nr_files / SIMPLEFS_FILES_PER_EXT;
    bi = eblock->nr_files % SIMPLEFS_FILES_PER_EXT / SIMPLEFS_FILES_PER_BLOCK;
    fi = eblock->nr_files % SIMPLEFS_FILES_PER_BLOCK;

    if (eblock->extents[ei].ee_start == 0) {
        bno = get_free_blocks(SIMPLEFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto end;
        }
        eblock->extents[ei].ee_start = bno;
        eblock->extents[ei].ee_len = 8;
        eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block +
                                                eblock->extents[ei - 1].ee_len
                                          : 0;
        alloc = true;
    }
    bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
    if (!bh2) {
        ret = -EIO;
        goto put_block;
    }
    dblock = (struct simplefs_dir_block *) bh2->b_data;

    dblock->files[fi].inode = inode->i_ino;
    strncpy(dblock->files[fi].filename, dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);

    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    inode_inc_link_count(inode);
    ihold(inode);
    d_instantiate(dentry, inode);
    return ret;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct simplefs_extent));
    }
end:
    brelse(bh);
    return ret;
}

#if MNT_IDMAP_REQUIRED()
static int simplefs_symlink(struct mnt_idmap *id,
                            struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
#elif USER_NS_REQUIRED()
static int simplefs_symlink(struct user_namespace *ns,
                            struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
#else
static int simplefs_symlink(struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
#endif
{
    struct super_block *sb = dir->i_sb;
    unsigned int l = strlen(symname) + 1;
    struct inode *inode = simplefs_new_inode(dir, S_IFLNK | S_IRWXUGO);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock = NULL;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    int ret = 0, alloc = false, bno = 0;
    int ei = 0, bi = 0, fi = 0;

    /* Check if symlink content is not too long */
    if (l > sizeof(ci->i_data))
        return -ENAMETOOLONG;

    /* fill directory data block */
    bh = sb_bread(sb, ci_dir->ei_block);
    if (!bh)
        return -EIO;
    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    if (eblock->nr_files == SIMPLEFS_MAX_SUBFILES) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    ei = eblock->nr_files / SIMPLEFS_FILES_PER_EXT;
    bi = eblock->nr_files % SIMPLEFS_FILES_PER_EXT / SIMPLEFS_FILES_PER_BLOCK;
    fi = eblock->nr_files % SIMPLEFS_FILES_PER_BLOCK;

    if (eblock->extents[ei].ee_start == 0) {
        bno = get_free_blocks(SIMPLEFS_SB(sb), 8);
        if (!bno) {
            ret = -ENOSPC;
            goto end;
        }
        eblock->extents[ei].ee_start = bno;
        eblock->extents[ei].ee_len = 8;
        eblock->extents[ei].ee_block = ei ? eblock->extents[ei - 1].ee_block +
                                                eblock->extents[ei - 1].ee_len
                                          : 0;
        alloc = true;
    }

    bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
    if (!bh2) {
        ret = -EIO;
        goto put_block;
    }

    dblock = (struct simplefs_dir_block *) bh2->b_data;
    dblock->files[fi].inode = inode->i_ino;
    strncpy(dblock->files[fi].filename, dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);

    eblock->nr_files++;
    mark_buffer_dirty(bh2);
    mark_buffer_dirty(bh);
    brelse(bh2);
    brelse(bh);

    inode->i_link = (char *) ci->i_data;
    memcpy(inode->i_link, symname, l);
    inode->i_size = l - 1;
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);
    return 0;

put_block:
    if (alloc && eblock->extents[ei].ee_start) {
        put_blocks(SIMPLEFS_SB(sb), eblock->extents[ei].ee_start,
                   eblock->extents[ei].ee_len);
        memset(&eblock->extents[ei], 0, sizeof(struct simplefs_extent));
    }

end:
    brelse(bh);
    return ret;
}

static const char *simplefs_get_link(struct dentry *dentry,
                                     struct inode *inode,
                                     struct delayed_call *done)
{
    return inode->i_link;
}

static const struct inode_operations simplefs_inode_ops = {
    .lookup = simplefs_lookup,
    .create = simplefs_create,
    .unlink = simplefs_unlink,
    .mkdir = simplefs_mkdir,
    .rmdir = simplefs_rmdir,
    .rename = simplefs_rename,
    .link = simplefs_link,
    .symlink = simplefs_symlink,
};

static const struct inode_operations symlink_inode_ops = {
    .get_link = simplefs_get_link,
};
