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
    inode->i_ctime.tv_sec = (time64_t) le32_to_cpu(cinode->i_ctime);
    inode->i_ctime.tv_nsec = 0;
    inode->i_atime.tv_sec = (time64_t) le32_to_cpu(cinode->i_atime);
    inode->i_atime.tv_nsec = 0;
    inode->i_mtime.tv_sec = (time64_t) le32_to_cpu(cinode->i_mtime);
    inode->i_mtime.tv_nsec = 0;
    inode->i_blocks = le32_to_cpu(cinode->i_blocks);
    set_nlink(inode, le32_to_cpu(cinode->i_nlink));

    if (S_ISDIR(inode->i_mode)) {
        ci->dir_block = le32_to_cpu(cinode->dir_block);
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

/*
 * Look for dentry in dir.
 * Fill dentry with NULL if not in dir, with the corresponding inode if found.
 * Returns NULL on success.
 */
static struct dentry *simplefs_lookup(struct inode *dir,
                                      struct dentry *dentry,
                                      unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct inode *inode = NULL;
    struct buffer_head *bh = NULL;
    struct simplefs_dir_block *dblock = NULL;
    struct simplefs_file *f = NULL;
    int i;

    /* Check filename length */
    if (dentry->d_name.len > SIMPLEFS_FILENAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    /* Read the directory block on disk */
    bh = sb_bread(sb, ci_dir->dir_block);
    if (!bh)
        return ERR_PTR(-EIO);
    dblock = (struct simplefs_dir_block *) bh->b_data;

    /* Search for the file in directory */
    for (i = 0; i < SIMPLEFS_MAX_SUBFILES; i++) {
        f = &dblock->files[i];
        if (!f->inode)
            break;
        if (!strncmp(f->filename, dentry->d_name.name, SIMPLEFS_FILENAME_LEN)) {
            inode = simplefs_iget(sb, f->inode);
            break;
        }
    }
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
        inode_init_owner(inode, dir, mode);
        set_nlink(inode, 1);
        inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);
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
    inode_init_owner(inode, dir, mode);
    inode->i_blocks = 1;
    if (S_ISDIR(mode)) {
        ci->dir_block = bno;
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

    inode->i_ctime = inode->i_atime = inode->i_mtime = current_time(inode);

    return inode;

put_inode:
    iput(inode);
put_ino:
    put_inode(sbi, ino);

    return ERR_PTR(ret);
}

/*
 * Create a file or directory in this way:
 *   - check filename length and if the parent directory is not full
 *   - create the new inode (allocate inode and blocks)
 *   - cleanup index block of the new inode
 *   - add new file/directory in parent index
 */
static int simplefs_create(struct inode *dir,
                           struct dentry *dentry,
                           umode_t mode,
                           bool excl)
{
    struct super_block *sb;
    struct inode *inode;
    struct simplefs_inode_info *ci_dir;
    struct simplefs_dir_block *dblock;
    char *fblock;
    struct buffer_head *bh, *bh2;
    int ret = 0, i;

    /* Check filename length */
    if (strlen(dentry->d_name.name) > SIMPLEFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Read parent directory index */
    ci_dir = SIMPLEFS_INODE(dir);
    sb = dir->i_sb;
    bh = sb_bread(sb, ci_dir->dir_block);
    if (!bh)
        return -EIO;

    dblock = (struct simplefs_dir_block *) bh->b_data;

    /* Check if parent directory is full */
    if (dblock->files[SIMPLEFS_MAX_SUBFILES - 1].inode != 0) {
        ret = -EMLINK;
        goto end;
    }

    /* Get a new free inode */
    inode = simplefs_new_inode(dir, mode);
    if (IS_ERR(inode)) {
        ret = PTR_ERR(inode);
        goto end;
    }

    /*
     * Scrub ei_block/dir_block for new file/directory to avoid previous data
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
    for (i = 0; i < SIMPLEFS_MAX_SUBFILES; i++)
        if (dblock->files[i].inode == 0)
            break;
    dblock->files[i].inode = inode->i_ino;
    strncpy(dblock->files[i].filename, dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);
    mark_buffer_dirty(bh);
    brelse(bh);

    /* Update stats and mark dir and new inode dirty */
    mark_inode_dirty(inode);
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
    if (S_ISDIR(mode))
        inc_nlink(dir);
    mark_inode_dirty(dir);

    /* setup dentry */
    d_instantiate(dentry, inode);

    return 0;

iput:
    put_blocks(SIMPLEFS_SB(sb), SIMPLEFS_INODE(inode)->ei_block, 1);
    put_inode(SIMPLEFS_SB(sb), inode->i_ino);
    iput(inode);
end:
    brelse(bh);
    return ret;
}

/*
 * Remove a link for a file including the reference in the parent directory.
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
    struct simplefs_dir_block *dir_block = NULL;
    struct simplefs_file_ei_block *file_block = NULL;
    int i, j, f_id = -1, nr_subs = 0;

    uint32_t ino = inode->i_ino;
    uint32_t bno = 0;

    /* Read parent directory index */
    bh = sb_bread(sb, SIMPLEFS_INODE(dir)->dir_block);
    if (!bh)
        return -EIO;
    dir_block = (struct simplefs_dir_block *) bh->b_data;

    /* Search for inode in parent index and get number of subfiles */
    for (i = 0; i < SIMPLEFS_MAX_SUBFILES; i++) {
        if (strncmp(dir_block->files[i].filename, dentry->d_name.name,
                    SIMPLEFS_FILENAME_LEN) == 0)
            f_id = i;
        else if (dir_block->files[i].inode == 0)
            break;
    }
    nr_subs = i;

    /* Remove file from parent directory */
    if (f_id != SIMPLEFS_MAX_SUBFILES - 1)
        memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
                (nr_subs - f_id - 1) * sizeof(struct simplefs_file));
    memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct simplefs_file));
    mark_buffer_dirty(bh);
    brelse(bh);

    if (S_ISLNK(inode->i_mode))
        goto clean_inode;

    /* Update inode stats */
    dir->i_mtime = dir->i_atime = dir->i_ctime = current_time(dir);
    if (S_ISDIR(inode->i_mode)) {
        drop_nlink(dir);
        drop_nlink(inode);
    }
    mark_inode_dirty(dir);

    if (inode->i_nlink > 1) {
        inode_dec_link_count(inode);
        return 0;
    }

    /*
     * Cleanup pointed blocks if unlinking a file. If we fail to read the
     * index block, cleanup inode anyway and lose this file's blocks
     * forever. If we fail to scrub a data block, don't fail (too late
     * anyway), just put the block and continue.
     */
    bno = SIMPLEFS_INODE(inode)->ei_block;
    bh = sb_bread(sb, bno);
    if (!bh)
        goto clean_inode;
    file_block = (struct simplefs_file_ei_block *) bh->b_data;
    if (S_ISDIR(inode->i_mode))
        goto scrub;
    for (i = 0; i < SIMPLEFS_MAX_EXTENTS; i++) {
        char *block;

        if (!file_block->extents[i].ee_start)
            break;

        put_blocks(sbi, file_block->extents[i].ee_start,
                   file_block->extents[i].ee_len);

        /* Scrub the extent */
        for (j = 0; j < file_block->extents[i].ee_len; j++) {
            bh2 = sb_bread(sb, file_block->extents[i].ee_start + j);
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
    inode->i_ctime.tv_sec = inode->i_mtime.tv_sec = inode->i_atime.tv_sec = 0;
    drop_nlink(inode);
    mark_inode_dirty(inode);

    /* Free inode and index block from bitmap */
    put_blocks(sbi, bno, 1);
    put_inode(sbi, ino);

    return 0;
}

static int simplefs_rename(struct inode *old_dir,
                           struct dentry *old_dentry,
                           struct inode *new_dir,
                           struct dentry *new_dentry,
                           unsigned int flags)
{
    struct super_block *sb = old_dir->i_sb;
    struct simplefs_inode_info *ci_old = SIMPLEFS_INODE(old_dir);
    struct simplefs_inode_info *ci_new = SIMPLEFS_INODE(new_dir);
    struct inode *src = d_inode(old_dentry);
    struct buffer_head *bh_old = NULL, *bh_new = NULL;
    struct simplefs_dir_block *dir_block = NULL;
    int i, f_id = -1, new_pos = -1, ret, nr_subs, f_pos = -1;

    /* fail with these unsupported flags */
    if (flags & (RENAME_EXCHANGE | RENAME_WHITEOUT))
        return -EINVAL;

    /* Check if filename is not too long */
    if (strlen(new_dentry->d_name.name) > SIMPLEFS_FILENAME_LEN)
        return -ENAMETOOLONG;

    /* Fail if new_dentry exists or if new_dir is full */
    bh_new = sb_bread(sb, ci_new->dir_block);
    if (!bh_new)
        return -EIO;
    dir_block = (struct simplefs_dir_block *) bh_new->b_data;
    for (i = 0; i < SIMPLEFS_MAX_SUBFILES; i++) {
        /* if old_dir == new_dir, save the renamed file position */
        if (new_dir == old_dir) {
            if (strncmp(dir_block->files[i].filename, old_dentry->d_name.name,
                        SIMPLEFS_FILENAME_LEN) == 0)
                f_pos = i;
        }
        if (strncmp(dir_block->files[i].filename, new_dentry->d_name.name,
                    SIMPLEFS_FILENAME_LEN) == 0) {
            ret = -EEXIST;
            goto relse_new;
        }
        if (new_pos < 0 && dir_block->files[i].inode == 0)
            new_pos = i;
    }
    /* if old_dir == new_dir, just rename entry */
    if (old_dir == new_dir) {
        strncpy(dir_block->files[f_pos].filename, new_dentry->d_name.name,
                SIMPLEFS_FILENAME_LEN);
        mark_buffer_dirty(bh_new);
        ret = 0;
        goto relse_new;
    }

    /* If new directory is full, fail */
    if (new_pos < 0) {
        ret = -EMLINK;
        goto relse_new;
    }

    /* insert in new parent directory */
    dir_block->files[new_pos].inode = src->i_ino;
    strncpy(dir_block->files[new_pos].filename, new_dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);
    mark_buffer_dirty(bh_new);
    brelse(bh_new);

    /* Update new parent inode metadata */
    new_dir->i_atime = new_dir->i_ctime = new_dir->i_mtime =
        current_time(new_dir);
    if (S_ISDIR(src->i_mode))
        inc_nlink(new_dir);
    mark_inode_dirty(new_dir);

    /* remove target from old parent directory */
    bh_old = sb_bread(sb, ci_old->dir_block);
    if (!bh_old)
        return -EIO;
    dir_block = (struct simplefs_dir_block *) bh_old->b_data;
    /* Search for inode in old directory and number of subfiles */
    for (i = 0; SIMPLEFS_MAX_SUBFILES; i++) {
        if (dir_block->files[i].inode == src->i_ino)
            f_id = i;
        else if (dir_block->files[i].inode == 0)
            break;
    }
    nr_subs = i;

    /* Remove file from old parent directory */
    if (f_id != SIMPLEFS_MAX_SUBFILES - 1)
        memmove(dir_block->files + f_id, dir_block->files + f_id + 1,
                (nr_subs - f_id - 1) * sizeof(struct simplefs_file));
    memset(&dir_block->files[nr_subs - 1], 0, sizeof(struct simplefs_file));
    mark_buffer_dirty(bh_old);
    brelse(bh_old);

    /* Update old parent inode metadata */
    old_dir->i_atime = old_dir->i_ctime = old_dir->i_mtime =
        current_time(old_dir);
    if (S_ISDIR(src->i_mode))
        drop_nlink(old_dir);
    mark_inode_dirty(old_dir);

    return 0;

relse_new:
    brelse(bh_new);
    return ret;
}

static int simplefs_mkdir(struct inode *dir,
                          struct dentry *dentry,
                          umode_t mode)
{
    return simplefs_create(dir, dentry, mode | S_IFDIR, 0);
}

static int simplefs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode = d_inode(dentry);
    struct buffer_head *bh;
    struct simplefs_dir_block *dblock;

    /* If the directory is not empty, fail */
    if (inode->i_nlink > 2)
        return -ENOTEMPTY;
    bh = sb_bread(sb, SIMPLEFS_INODE(inode)->dir_block);
    if (!bh)
        return -EIO;
    dblock = (struct simplefs_dir_block *) bh->b_data;
    if (dblock->files[0].inode != 0) {
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
    struct simplefs_dir_block *dir_block;
    struct buffer_head *bh;
    int f_pos = -1, ret = 0, i = 0;

    bh = sb_bread(sb, ci_dir->dir_block);
    if (!bh)
        return -EIO;
    dir_block = (struct simplefs_dir_block *) bh->b_data;

    if (dir_block->files[SIMPLEFS_MAX_SUBFILES - 1].inode != 0) {
        ret = -EMLINK;
        printk(KERN_INFO "directory is full");
        goto end;
    }

    for (i = 0; i < SIMPLEFS_MAX_SUBFILES; i++) {
        if (dir_block->files[i].inode == 0) {
            f_pos = i;
            break;
        }
    }

    dir_block->files[f_pos].inode = inode->i_ino;
    strncpy(dir_block->files[f_pos].filename, dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);
    mark_buffer_dirty(bh);

    inode_inc_link_count(inode);
    d_instantiate(dentry, inode);
end:
    brelse(bh);
    return ret;
}

static int simplefs_symlink(struct inode *dir,
                            struct dentry *dentry,
                            const char *symname)
{
    struct super_block *sb = dir->i_sb;
    unsigned int l = strlen(symname) + 1;
    struct inode *inode = simplefs_new_inode(dir, S_IFLNK | S_IRWXUGO);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct simplefs_inode_info *ci_dir = SIMPLEFS_INODE(dir);
    struct simplefs_dir_block *dir_block;
    struct buffer_head *bh;
    int f_pos = 0, i = 0;

    /* Check if symlink content is not too long */
    if (l > sizeof(ci->i_data))
        return -ENAMETOOLONG;

    /* fill directory data block */
    bh = sb_bread(sb, ci_dir->dir_block);

    if (!bh)
        return -EIO;
    dir_block = (struct simplefs_dir_block *) bh->b_data;

    if (dir_block->files[SIMPLEFS_MAX_SUBFILES - 1].inode != 0) {
        printk(KERN_INFO "directory is full\n");
        return -EMLINK;
    }

    for (i = 0; i < SIMPLEFS_MAX_SUBFILES; i++) {
        if (dir_block->files[i].inode == 0) {
            f_pos = i;
            break;
        }
    }

    dir_block->files[f_pos].inode = inode->i_ino;
    strncpy(dir_block->files[f_pos].filename, dentry->d_name.name,
            SIMPLEFS_FILENAME_LEN);
    mark_buffer_dirty(bh);
    brelse(bh);

    inode->i_link = (char *) ci->i_data;
    memcpy(inode->i_link, symname, l);
    inode->i_size = l - 1;
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);

    return 0;
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
