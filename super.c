#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/statfs.h>

#include <linux/blkdev.h>
#include <linux/jbd2.h>
#include <linux/namei.h>
#include <linux/parser.h>

#include "simplefs.h"

struct dentry *simplefs_mount(struct file_system_type *fs_type,
                              int flags,
                              const char *dev_name,
                              void *data);
void simplefs_kill_sb(struct super_block *sb);
static struct kmem_cache *simplefs_inode_cache;

/* Needed to initiate the inode cache, to allow us to attach
 * filesystem-specific inode information.
 */
int simplefs_init_inode_cache(void)
{
    simplefs_inode_cache = kmem_cache_create_usercopy(
        "simplefs_cache", sizeof(struct simplefs_inode_info), 0, 0, 0,
        sizeof(struct simplefs_inode_info), NULL);
    if (!simplefs_inode_cache)
        return -ENOMEM;
    return 0;
}

/* De-allocate the inode cache */
void simplefs_destroy_inode_cache(void)
{
    /* wait for call_rcu() and prevent the free cache be used */
    rcu_barrier();

    kmem_cache_destroy(simplefs_inode_cache);
}

static struct inode *simplefs_alloc_inode(struct super_block *sb)
{
    struct simplefs_inode_info *ci =
        kmem_cache_alloc(simplefs_inode_cache, GFP_KERNEL);
    if (!ci)
        return NULL;

    inode_init_once(&ci->vfs_inode);
    return &ci->vfs_inode;
}

static void simplefs_destroy_inode(struct inode *inode)
{
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    kmem_cache_free(simplefs_inode_cache, ci);
}

static int simplefs_write_inode(struct inode *inode,
                                struct writeback_control *wbc)
{
    struct simplefs_inode *disk_inode;
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct super_block *sb = inode->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    uint32_t ino = inode->i_ino;
    uint32_t inode_block = (ino / SIMPLEFS_INODES_PER_BLOCK) + 1;
    uint32_t inode_shift = ino % SIMPLEFS_INODES_PER_BLOCK;

    if (ino >= sbi->nr_inodes)
        return 0;

    bh = sb_bread(sb, inode_block);
    if (!bh)
        return -EIO;

    disk_inode = (struct simplefs_inode *) bh->b_data;
    disk_inode += inode_shift;

    /* update the mode using what the generic inode has */
    disk_inode->i_mode = inode->i_mode;
    disk_inode->i_uid = i_uid_read(inode);
    disk_inode->i_gid = i_gid_read(inode);
    disk_inode->i_size = inode->i_size;

#if SIMPLEFS_AT_LEAST(6, 6, 0)
    struct timespec64 ctime = inode_get_ctime(inode);
    disk_inode->i_ctime = ctime.tv_sec;
#else
    disk_inode->i_ctime = inode->i_ctime.tv_sec;
#endif

#if SIMPLEFS_AT_LEAST(6, 7, 0)
    disk_inode->i_atime = inode_get_atime_sec(inode);
    disk_inode->i_atime = inode_get_mtime_sec(inode);
#else
    disk_inode->i_atime = inode->i_atime.tv_sec;
    disk_inode->i_mtime = inode->i_mtime.tv_sec;
#endif
    disk_inode->i_blocks = inode->i_blocks;
    disk_inode->i_nlink = inode->i_nlink;
    disk_inode->ei_block = ci->ei_block;
    strncpy(disk_inode->i_data, ci->i_data, sizeof(ci->i_data));

    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    return 0;
}

static void simplefs_put_super(struct super_block *sb)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    int aborted = 0;
    int err;

    if (sbi->journal) {
        aborted = is_journal_aborted(sbi->journal);
        err = jbd2_journal_destroy(sbi->journal);
        sbi->journal = NULL;
        if ((err < 0) && !aborted) {
            pr_err("Couldn't clean up the journal, error %d\n", -err);
        }
    }

    sync_blockdev(sb->s_bdev);
    invalidate_bdev(sb->s_bdev);
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    if (sbi->s_journal_bdev_file) {
        sync_blockdev(file_bdev(sbi->s_journal_bdev_file));
        invalidate_bdev(file_bdev(sbi->s_journal_bdev_file));
    }
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    if (sbi->s_journal_bdev_handle) {
        sync_blockdev(sbi->s_journal_bdev_handle->bdev);
        invalidate_bdev(sbi->s_journal_bdev_handle->bdev);
    }
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    if (sbi->s_journal_bdev) {
        sync_blockdev(sbi->s_journal_bdev);
        invalidate_bdev(sbi->s_journal_bdev);
    }
#elif SIMPLEFS_AT_LEAST(6, 5, 0)
    if (sbi->s_journal_bdev) {
        sync_blockdev(sbi->s_journal_bdev);
        invalidate_bdev(sbi->s_journal_bdev);
        blkdev_put(sbi->s_journal_bdev, sb);
        sbi->s_journal_bdev = NULL;
    }
#elif SIMPLEFS_AT_LEAST(5, 10, 0)
    if (sbi->s_journal_bdev && sbi->s_journal_bdev != sb->s_bdev) {
        sync_blockdev(sbi->s_journal_bdev);
        invalidate_bdev(sbi->s_journal_bdev);
        blkdev_put(sbi->s_journal_bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
        sbi->s_journal_bdev = NULL;
    }
#endif

    if (sbi) {
        kfree(sbi->ifree_bitmap);
        kfree(sbi->bfree_bitmap);
        kfree(sbi);
    }
}

static int simplefs_sync_fs(struct super_block *sb, int wait)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_sb_info *disk_sb;
    int i;

    /* Flush superblock */
    struct buffer_head *bh = sb_bread(sb, 0);
    if (!bh)
        return -EIO;

    disk_sb = (struct simplefs_sb_info *) bh->b_data;

    disk_sb->nr_blocks = sbi->nr_blocks;
    disk_sb->nr_inodes = sbi->nr_inodes;
    disk_sb->nr_istore_blocks = sbi->nr_istore_blocks;
    disk_sb->nr_ifree_blocks = sbi->nr_ifree_blocks;
    disk_sb->nr_bfree_blocks = sbi->nr_bfree_blocks;
    disk_sb->nr_free_inodes = sbi->nr_free_inodes;
    disk_sb->nr_free_blocks = sbi->nr_free_blocks;

    mark_buffer_dirty(bh);
    if (wait)
        sync_dirty_buffer(bh);
    brelse(bh);

    /* Flush free inodes bitmask */
    for (i = 0; i < sbi->nr_ifree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->ifree_bitmap + i * SIMPLEFS_BLOCK_SIZE,
               SIMPLEFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    /* Flush free blocks bitmask */
    for (i = 0; i < sbi->nr_bfree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh)
            return -EIO;

        memcpy(bh->b_data, (void *) sbi->bfree_bitmap + i * SIMPLEFS_BLOCK_SIZE,
               SIMPLEFS_BLOCK_SIZE);

        mark_buffer_dirty(bh);
        if (wait)
            sync_dirty_buffer(bh);
        brelse(bh);
    }

    return 0;
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *stat)
{
    struct super_block *sb = dentry->d_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);

    stat->f_type = SIMPLEFS_MAGIC;
    stat->f_bsize = SIMPLEFS_BLOCK_SIZE;
    stat->f_blocks = sbi->nr_blocks;
    stat->f_bfree = sbi->nr_free_blocks;
    stat->f_bavail = sbi->nr_free_blocks;
    stat->f_files = sbi->nr_inodes;
    stat->f_ffree = sbi->nr_free_inodes;
    stat->f_namelen = SIMPLEFS_FILENAME_LEN;

    return 0;
}

/* Code related to the external journal device settings */

static journal_t *simplefs_get_dev_journal(struct super_block *sb,
                                           dev_t journal_dev)
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct buffer_head *bh;
    struct block_device *bdev;
    int hblock, blocksize;
    unsigned long long sb_block, start, len;
    unsigned long offset;
    journal_t *journal;
    int errno = 0;
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    struct file *bdev_file;
    bdev_file = bdev_file_open_by_dev(
        journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_RESTRICT_WRITES,
        sb, &fs_holder_ops);
#elif SIMPLEFS_AT_LEAST(6, 8, 0)
    struct bdev_handle *bdev_handle;
    bdev_handle = bdev_open_by_dev(
        journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE | BLK_OPEN_RESTRICT_WRITES,
        sb, &fs_holder_ops);
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    struct bdev_handle *bdev_handle;
    up_write(&sb->s_umount);
    bdev_handle = bdev_open_by_dev(journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE,
                                   sb, &fs_holder_ops);
    down_write(&sb->s_umount);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    up_write(&sb->s_umount);
    bdev = blkdev_get_by_dev(journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE, sb,
                             &fs_holder_ops);
    down_write(&sb->s_umount);
#elif SIMPLEFS_AT_LEAST(6, 5, 0)
    bdev = blkdev_get_by_dev(journal_dev, BLK_OPEN_READ | BLK_OPEN_WRITE, sb,
                             NULL);
#elif SIMPLEFS_AT_LEAST(5, 10, 0)
    bdev = blkdev_get_by_dev(journal_dev, FMODE_READ | FMODE_WRITE | FMODE_EXCL,
                             sb);
#endif

#if SIMPLEFS_AT_LEAST(6, 9, 0)
    if (IS_ERR(bdev_file)) {
        printk(KERN_ERR
               "failed to open journal device unknown-block(%u,%u) %ld\n",
               MAJOR(journal_dev), MINOR(journal_dev), PTR_ERR(bdev_file));
        return ERR_CAST(bdev_file);
    }
    bdev = file_bdev(bdev_file);
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    if (IS_ERR(bdev_handle)) {
        printk(KERN_ERR
               "failed to open journal device unknown-block(%u,%u) %ld\n",
               MAJOR(journal_dev), MINOR(journal_dev), PTR_ERR(bdev_handle));
        return ERR_CAST(bdev_handle);
    }
    bdev = bdev_handle->bdev;
#elif SIMPLEFS_AT_LEAST(5, 10, 0)
    if (IS_ERR(bdev)) {
        printk(KERN_ERR "failed to open block device (%u:%u), error: %ld\n",
               MAJOR(journal_dev), MINOR(journal_dev), PTR_ERR(bdev));
        return ERR_CAST(bdev);
    }
#endif

    blocksize = sb->s_blocksize;
    hblock = bdev_logical_block_size(bdev);

    if (blocksize < hblock) {
        pr_err("blocksize too small for journal device\n");
        errno = -EINVAL;
        goto out_bdev;
    }

    sb_block = SIMPLEFS_BLOCK_SIZE / blocksize;
    offset = SIMPLEFS_BLOCK_SIZE % blocksize;

#if SIMPLEFS_AT_LEAST(6, 9, 0)
    set_blocksize(bdev_file, blocksize);
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    set_blocksize(bdev, blocksize);
#endif
    bh = __bread(bdev, sb_block, blocksize);

    if (!bh) {
        pr_err("couldn't read superblock of external journal\n");
        errno = -EINVAL;
        goto out_bdev;
    }
    /*
     * FIXME: Currently, the exact size of the external journal device is not
     * available. Therefore, we use the device size divided by the block size to
     * set `len`. Hint: External device size available now is 8MB.
     *
     * Future implementation might need to change to:
     * len = CapacityOfJournalDevice / SIMPLEFS_BLOCK_SIZE
     */

    len = 2048;
    start = sb_block;
    brelse(bh);

#if SIMPLEFS_AT_LEAST(6, 9, 0)
    journal = jbd2_journal_init_dev(file_bdev(bdev_file), sb->s_bdev, start,
                                    len, sb->s_blocksize);
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    journal = jbd2_journal_init_dev(bdev_handle->bdev, sb->s_bdev, start, len,
                                    sb->s_blocksize);
#elif SIMPLEFS_AT_LEAST(5, 15, 0)
    journal = jbd2_journal_init_dev(bdev, sb->s_bdev, start, len, blocksize);
#endif

    if (IS_ERR(journal)) {
        pr_err(
            "simplefs_get_dev_journal: failed to initialize journal, error "
            "%ld\n",
            PTR_ERR(journal));
        errno = PTR_ERR(journal);
        goto out_bdev;
    }
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    sbi->s_journal_bdev_file = bdev_file;
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    sbi->s_journal_bdev_handle = bdev_handle;
#elif SIMPLEFS_AT_LEAST(5, 15, 0)
    sbi->s_journal_bdev = bdev;
#endif

    journal->j_private = sb;
    return journal;

out_bdev:
#if SIMPLEFS_AT_LEAST(6, 9, 0)
    fput(bdev_file);
#elif SIMPLEFS_AT_LEAST(6, 7, 0)
    bdev_release(bdev_handle);
#elif SIMPLEFS_AT_LEAST(6, 5, 0)
    blkdev_put(bdev, sb);
#elif SIMPLEFS_AT_LEAST(5, 10, 0)
    blkdev_put(bdev, FMODE_READ | FMODE_WRITE | FMODE_EXCL);
#endif
    return NULL;
}

static int simplefs_load_journal(struct super_block *sb,
                                 unsigned long journal_devnum)
{
    journal_t *journal;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    dev_t journal_dev;
    int err = 0;
    int really_read_only;
    int journal_dev_ro;

    journal_dev = new_decode_dev(journal_devnum);
    journal = simplefs_get_dev_journal(sb, journal_dev);
    if (IS_ERR(journal)) {
        pr_err("Failed to get journal from device, error %ld\n",
               PTR_ERR(journal));
        return PTR_ERR(journal);
    }

    journal_dev_ro = bdev_read_only(journal->j_dev);
    really_read_only = bdev_read_only(sb->s_bdev) | journal_dev_ro;

    if (journal_dev_ro && !sb_rdonly(sb)) {
        pr_err("journal device read-only, try mounting with '-o ro'\n");
        err = -EROFS;
        goto err_out;
    }

    err = jbd2_journal_wipe(journal, !really_read_only);

    if (!err) {
        err = jbd2_journal_load(journal);
        if (err) {
            pr_err("error loading journal, error %d\n", err);
            goto err_out;
        }
    }

    sbi->journal = journal;

    return 0;

err_out:
    jbd2_journal_destroy(journal);
    return err;
}

/* we use SIMPLEFS_OPT_JOURNAL_PATH case to load external journal device now */
#define SIMPLEFS_OPT_JOURNAL_DEV 1
#define SIMPLEFS_OPT_JOURNAL_PATH 2
static const match_table_t tokens = {
    {SIMPLEFS_OPT_JOURNAL_DEV, "journal_dev=%u"},
    {SIMPLEFS_OPT_JOURNAL_PATH, "journal_path=%s"},
};
static int simplefs_parse_options(struct super_block *sb, char *options)
{
    substring_t args[MAX_OPT_ARGS];
    int token, ret = 0, arg;
    char *p;
    char *journal_path;
    struct inode *journal_inode;
    struct path path;

    pr_info("simplefs_parse_options: parsing options '%s'\n", options);

    while ((p = strsep(&options, ","))) {
        if (!*p)
            continue;

        args[0].to = args[0].from = NULL;
        token = match_token(p, tokens, args);

        switch (token) {
        case SIMPLEFS_OPT_JOURNAL_DEV:
            if (args->from && match_int(args, &arg)) {
                pr_err("simplefs_parse_options: match_int failed\n");
                return 1;
            }
            if ((ret = simplefs_load_journal(sb, arg))) {
                pr_err(
                    "simplefs_parse_options: simplefs_load_journal failed with "
                    "%d\n",
                    ret);
                return ret;
            }
            break;

        case SIMPLEFS_OPT_JOURNAL_PATH: {
            journal_path = match_strdup(&args[0]);
            if (!journal_path) {
                pr_err("simplefs_parse_options: match_strdup failed\n");
                return -ENOMEM;
            }
            ret = kern_path(journal_path, LOOKUP_FOLLOW, &path);
            if (ret) {
                pr_err(
                    "simplefs_parse_options: kern_path failed with error %d\n",
                    ret);
                kfree(journal_path);
                return ret;
            }

            journal_inode = path.dentry->d_inode;

            path_put(&path);
            kfree(journal_path);

            if (S_ISBLK(journal_inode->i_mode)) {
                unsigned long journal_devnum =
                    new_encode_dev(journal_inode->i_rdev);
                if ((ret = simplefs_load_journal(sb, journal_devnum))) {
                    pr_err(
                        "simplefs_parse_options: simplefs_load_journal failed "
                        "with %d\n",
                        ret);
                    return ret;
                }
            }
            break;
        }
        }
    }

    return 0;
}

static struct super_operations simplefs_super_ops = {
    .put_super = simplefs_put_super,
    .alloc_inode = simplefs_alloc_inode,
    .destroy_inode = simplefs_destroy_inode,
    .write_inode = simplefs_write_inode,
    .sync_fs = simplefs_sync_fs,
    .statfs = simplefs_statfs,
};

/* Fill the struct superblock from partition superblock */
int simplefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct buffer_head *bh = NULL;
    struct simplefs_sb_info *csb = NULL;
    struct simplefs_sb_info *sbi = NULL;
    struct inode *root_inode = NULL;
    int ret = 0, i;

    /* Initialize the superblock */
    sb->s_magic = SIMPLEFS_MAGIC;
    sb_set_blocksize(sb, SIMPLEFS_BLOCK_SIZE);
    sb->s_maxbytes = SIMPLEFS_MAX_FILESIZE;
    sb->s_op = &simplefs_super_ops;

    /* Read the superblock from disk */
    bh = sb_bread(sb, SIMPLEFS_SB_BLOCK_NR);
    if (!bh)
        return -EIO;

    csb = (struct simplefs_sb_info *) bh->b_data;

    /* Check magic number */
    if (csb->magic != sb->s_magic) {
        pr_err("Wrong magic number\n");
        ret = -EINVAL;
        goto release;
    }

    /* Allocate sb_info */
    sbi = kzalloc(sizeof(struct simplefs_sb_info), GFP_KERNEL);
    if (!sbi) {
        ret = -ENOMEM;
        goto release;
    }

    sbi->nr_blocks = csb->nr_blocks;
    sbi->nr_inodes = csb->nr_inodes;
    sbi->nr_istore_blocks = csb->nr_istore_blocks;
    sbi->nr_ifree_blocks = csb->nr_ifree_blocks;
    sbi->nr_bfree_blocks = csb->nr_bfree_blocks;
    sbi->nr_free_inodes = csb->nr_free_inodes;
    sbi->nr_free_blocks = csb->nr_free_blocks;
    sb->s_fs_info = sbi;

    brelse(bh);

    /* Allocate and copy ifree_bitmap */
    sbi->ifree_bitmap =
        kzalloc(sbi->nr_ifree_blocks * SIMPLEFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->ifree_bitmap) {
        ret = -ENOMEM;
        goto free_sbi;
    }

    for (i = 0; i < sbi->nr_ifree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_ifree;
        }

        memcpy((void *) sbi->ifree_bitmap + i * SIMPLEFS_BLOCK_SIZE, bh->b_data,
               SIMPLEFS_BLOCK_SIZE);

        brelse(bh);
    }
    bh = NULL;

    /* Allocate and copy bfree_bitmap */
    sbi->bfree_bitmap =
        kzalloc(sbi->nr_bfree_blocks * SIMPLEFS_BLOCK_SIZE, GFP_KERNEL);
    if (!sbi->bfree_bitmap) {
        ret = -ENOMEM;
        goto free_ifree;
    }

    for (i = 0; i < sbi->nr_bfree_blocks; i++) {
        int idx = sbi->nr_istore_blocks + sbi->nr_ifree_blocks + i + 1;

        bh = sb_bread(sb, idx);
        if (!bh) {
            ret = -EIO;
            goto free_bfree;
        }

        memcpy((void *) sbi->bfree_bitmap + i * SIMPLEFS_BLOCK_SIZE, bh->b_data,
               SIMPLEFS_BLOCK_SIZE);

        brelse(bh);
    }

    bh = NULL;
    /* Create root inode */
    root_inode = simplefs_iget(sb, 1);
    if (IS_ERR(root_inode)) {
        ret = PTR_ERR(root_inode);
        goto free_bfree;
    }

#if SIMPLEFS_AT_LEAST(6, 3, 0)
    inode_init_owner(&nop_mnt_idmap, root_inode, NULL, root_inode->i_mode);
#elif SIMPLEFS_AT_LEAST(5, 12, 0)
    inode_init_owner(&init_user_ns, root_inode, NULL, root_inode->i_mode);
#else
    inode_init_owner(root_inode, NULL, root_inode->i_mode);
#endif

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        ret = -ENOMEM;
        goto iput;
    }

    ret = simplefs_parse_options(sb, data);
    if (ret) {
        pr_err("simplefs_fill_super: Failed to parse options, error code: %d\n",
               ret);
        return ret;
    }

    return 0;

iput:
    iput(root_inode);
free_bfree:
    kfree(sbi->bfree_bitmap);
free_ifree:
    kfree(sbi->ifree_bitmap);
free_sbi:
    kfree(sbi);
release:
    brelse(bh);

    return ret;
}
