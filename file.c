#define pr_fmt(fmt) "simplefs: " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mpage.h>

#include "bitmap.h"
#include "simplefs.h"

/* Associate the provided 'buffer_head' parameter with the iblock-th block of
 * the file denoted by inode. Should the specified block be unallocated and the
 * create flag is set to true, proceed to allocate a new block on the disk and
 * establish a mapping for it.
 */
static int simplefs_file_get_block(struct inode *inode,
                                   sector_t iblock,
                                   struct buffer_head *bh_result,
                                   int create)
{
    struct super_block *sb = inode->i_sb;
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(sb);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct simplefs_file_ei_block *index;
    struct buffer_head *bh_index;
    int ret = 0, bno;
    uint32_t extent;

    /* If block number exceeds filesize, fail */
    if (iblock >= SIMPLEFS_MAX_BLOCKS_PER_EXTENT * SIMPLEFS_MAX_EXTENTS)
        return -EFBIG;

    /* Read directory block from disk */
    bh_index = sb_bread(sb, ci->ei_block);
    if (!bh_index)
        return -EIO;
    index = (struct simplefs_file_ei_block *) bh_index->b_data;

    extent = simplefs_ext_search(index, iblock);
    if (extent == -1) {
        ret = -EFBIG;
        goto brelse_index;
    }

    /* Determine whether the 'iblock' is currently allocated. If it is not and
     * the create parameter is set to true, then allocate the block. Otherwise,
     * retrieve the physical block number.
     */
    if (index->extents[extent].ee_start == 0) {
        if (!create) {
            ret = 0;
            goto brelse_index;
        }
        bno = get_free_blocks(sbi, 8);
        if (!bno) {
            ret = -ENOSPC;
            goto brelse_index;
        }

        index->extents[extent].ee_start = bno;
        index->extents[extent].ee_len = 8;
        index->extents[extent].ee_block =
            extent ? index->extents[extent - 1].ee_block +
                         index->extents[extent - 1].ee_len
                   : 0;
    } else {
        bno = index->extents[extent].ee_start + iblock -
              index->extents[extent].ee_block;
    }

    /* Map the physical block to the given 'buffer_head'. */
    map_bh(bh_result, sb, bno);

brelse_index:
    brelse(bh_index);

    return ret;
}

/* Called by the page cache to read a page from the physical disk and map it
 * into memory.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static void simplefs_readahead(struct readahead_control *rac)
{
    mpage_readahead(rac, simplefs_file_get_block);
}
#else
static int simplefs_readpage(struct file *file, struct page *page)
{
    return mpage_readpage(page, simplefs_file_get_block);
}
#endif

/* Called by the page cache to write a dirty page to the physical disk (when
 * sync is called or when memory is needed).
 */
static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
    return block_write_full_page(page, simplefs_file_get_block, wbc);
}

/* Called by the VFS when a write() syscall is made on a file, before writing
 * the data into the page cache. This function checks if the write operation
 * can complete and allocates the necessary blocks through block_write_begin().
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
static int simplefs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                struct page **pagep,
                                void **fsdata)
#else
static int simplefs_write_begin(struct file *file,
                                struct address_space *mapping,
                                loff_t pos,
                                unsigned int len,
                                unsigned int flags,
                                struct page **pagep,
                                void **fsdata)
#endif
{
    struct simplefs_sb_info *sbi = SIMPLEFS_SB(file->f_inode->i_sb);
    int err;
    uint32_t nr_allocs = 0;

    /* Check if the write can be completed (enough space?) */
    if (pos + len > SIMPLEFS_MAX_FILESIZE)
        return -ENOSPC;

    nr_allocs = max(pos + len, file->f_inode->i_size) / SIMPLEFS_BLOCK_SIZE;
    if (nr_allocs > file->f_inode->i_blocks - 1)
        nr_allocs -= file->f_inode->i_blocks - 1;
    else
        nr_allocs = 0;
    if (nr_allocs > sbi->nr_free_blocks)
        return -ENOSPC;

        /* prepare the write */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
    err = block_write_begin(mapping, pos, len, pagep, simplefs_file_get_block);
#else
    err = block_write_begin(mapping, pos, len, flags, pagep,
                            simplefs_file_get_block);
#endif
    /* if this failed, reclaim newly allocated blocks */
    if (err < 0)
        pr_err("newly allocated blocks reclaim not implemented yet\n");
    return err;
}

/* Called by the VFS after writing data from a write() syscall to the page
 * cache. This function updates inode metadata and truncates the file if
 * necessary.
 */
static int simplefs_write_end(struct file *file,
                              struct address_space *mapping,
                              loff_t pos,
                              unsigned int len,
                              unsigned int copied,
                              struct page *page,
                              void *fsdata)
{
    struct inode *inode = file->f_inode;
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct super_block *sb = inode->i_sb;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
    struct timespec64 cur_time;
#endif
    uint32_t nr_blocks_old;

    /* Complete the write() */
    int ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
    if (ret < len) {
        pr_err("wrote less than requested.");
        return ret;
    }

    nr_blocks_old = inode->i_blocks;

    /* Update inode metadata */
    inode->i_blocks = inode->i_size / SIMPLEFS_BLOCK_SIZE + 2;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)
    cur_time = current_time(inode);
    inode->i_mtime = cur_time;
    inode_set_ctime_to_ts(inode, cur_time);
#else
    inode->i_mtime = inode->i_ctime = current_time(inode);
#endif

    mark_inode_dirty(inode);

    /* If file is smaller than before, free unused blocks */
    if (nr_blocks_old > inode->i_blocks) {
        int i;
        struct buffer_head *bh_index;
        struct simplefs_file_ei_block *index;
        uint32_t first_ext;

        /* Free unused blocks from page cache */
        truncate_pagecache(inode, inode->i_size);

        /* Read ei_block to remove unused blocks */
        bh_index = sb_bread(sb, ci->ei_block);
        if (!bh_index) {
            pr_err("failed truncating '%s'. we just lost %llu blocks\n",
                   file->f_path.dentry->d_name.name,
                   nr_blocks_old - inode->i_blocks);
            goto end;
        }
        index = (struct simplefs_file_ei_block *) bh_index->b_data;

        first_ext = simplefs_ext_search(index, inode->i_blocks - 1);

        /* Reserve unused block in last extent */
        if (inode->i_blocks - 1 != index->extents[first_ext].ee_block)
            first_ext++;

        for (i = first_ext; i < SIMPLEFS_MAX_EXTENTS; i++) {
            if (!index->extents[i].ee_start)
                break;
            put_blocks(SIMPLEFS_SB(sb), index->extents[i].ee_start,
                       index->extents[i].ee_len);
            memset(&index->extents[i], 0, sizeof(struct simplefs_extent));
        }
        mark_buffer_dirty(bh_index);
        brelse(bh_index);
    }
end:
    return ret;
}

const struct address_space_operations simplefs_aops = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 19, 0)
    .readahead = simplefs_readahead,
#else
    .readpage = simplefs_readpage,
#endif
    .writepage = simplefs_writepage,
    .write_begin = simplefs_write_begin,
    .write_end = simplefs_write_end,
};

const struct file_operations simplefs_file_ops = {
    .llseek = generic_file_llseek,
    .owner = THIS_MODULE,
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .fsync = generic_file_fsync,
};
