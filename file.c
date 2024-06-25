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
        bno = get_free_blocks(sb, 8);
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
#if SIMPLEFS_AT_LEAST(5, 19, 0)
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
#if SIMPLEFS_AT_LEAST(6, 8, 0)
static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
    struct folio *folio = page_folio(page);
    return __block_write_full_folio(page->mapping->host, folio,
                                    simplefs_file_get_block, wbc);
}
#else
static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
    return block_write_full_page(page, simplefs_file_get_block, wbc);
}
#endif

/* Called by the VFS when a write() syscall is made on a file, before writing
 * the data into the page cache. This function checks if the write operation
 * can complete and allocates the necessary blocks through block_write_begin().
 */
#if SIMPLEFS_AT_LEAST(5, 19, 0)
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
#if SIMPLEFS_AT_LEAST(5, 19, 0)
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
#if SIMPLEFS_AT_LEAST(6, 6, 0)
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
    inode->i_blocks = DIV_ROUND_UP(inode->i_size, SIMPLEFS_BLOCK_SIZE) + 1;

#if SIMPLEFS_AT_LEAST(6, 7, 0)
    cur_time = current_time(inode);
    inode_set_mtime_to_ts(inode, cur_time);
    inode_set_ctime_to_ts(inode, cur_time);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
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
            pr_err("Failed to truncate '%s'. Lost %llu blocks\n",
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

/*
 * Called when a file is opened in the simplefs.
 * It checks the flags associated with the file opening mode (O_WRONLY, O_RDWR,
 * O_TRUNC) and performs truncation if the file is being opened for write or
 * read/write and the O_TRUNC flag is set.
 *
 * Truncation is achieved by reading the file's index block from disk, iterating
 * over the data block pointers, releasing the associated data blocks, and
 * updating the inode metadata (size and block count).
 */
static int simplefs_open(struct inode *inode, struct file *filp)
{
    bool wronly = (filp->f_flags & O_WRONLY);
    bool rdwr = (filp->f_flags & O_RDWR);
    bool trunc = (filp->f_flags & O_TRUNC);

    if ((wronly || rdwr) && trunc && inode->i_size) {
        struct buffer_head *bh_index;
        struct simplefs_file_ei_block *ei_block;
        sector_t iblock;

        /* Fetch the file's extent block from disk */
        bh_index = sb_bread(inode->i_sb, SIMPLEFS_INODE(inode)->ei_block);
        if (!bh_index)
            return -EIO;

        ei_block = (struct simplefs_file_ei_block *) bh_index->b_data;

        for (iblock = 0; iblock <= SIMPLEFS_MAX_EXTENTS &&
                         ei_block->extents[iblock].ee_start;
             iblock++) {
            put_blocks(SIMPLEFS_SB(inode->i_sb),
                       ei_block->extents[iblock].ee_start,
                       ei_block->extents[iblock].ee_len);
            memset(&ei_block->extents[iblock], 0,
                   sizeof(struct simplefs_extent));
        }
        /* Update inode metadata */
        inode->i_size = 0;
        inode->i_blocks = 1;

        mark_buffer_dirty(bh_index);
        brelse(bh_index);
        mark_inode_dirty(inode);
    }
    return 0;
}

static ssize_t simplefs_read(struct file *file,
                             char __user *buf,
                             size_t len,
                             loff_t *ppos)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    ssize_t bytes_read = 0;
    loff_t pos = *ppos;

    if (pos > inode->i_size)
        return 0;

    /* find extent block */
    struct buffer_head *bh = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    struct simplefs_file_ei_block *ei_block =
        (struct simplefs_file_ei_block *) bh->b_data;

    if (pos + len > inode->i_size)
        len = inode->i_size - pos;

    /* count block position */
    sector_t block_index = pos / SIMPLEFS_BLOCK_SIZE;
    sector_t ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    sector_t block_offset = ei_block->extents[ei_index].ee_start +
                            block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT;

    while (len > 0) {
        struct buffer_head *bh_data = sb_bread(sb, block_offset);
        if (!bh_data) {
            pr_err("Failed to read data block %llu\n", block_offset);
            bytes_read = -EIO;
            break;
        }

        size_t offset = pos % SIMPLEFS_BLOCK_SIZE;
        size_t bytes_to_read =
            min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - pos % SIMPLEFS_BLOCK_SIZE);
        if (copy_to_user(buf + bytes_read, bh_data->b_data + offset,
                         bytes_to_read)) {
            brelse(bh_data);
            bytes_read = -EFAULT;
            break;
        }
        brelse(bh_data);

        /* successfully read data */
        bytes_read += bytes_to_read;
        len -= bytes_to_read;
        pos += bytes_to_read;

        /* count extent block */
        block_index++;
        ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
        block_offset = ei_block->extents[ei_index].ee_start +
                       block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    }

    brelse(bh);
    *ppos = pos;

    return bytes_read;
}

static ssize_t simplefs_write(struct file *file,
                              const char __user *buf,
                              size_t len,
                              loff_t *ppos)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    ssize_t bytes_write = 0;
    loff_t pos = *ppos;

    if (pos > inode->i_size)
        return 0;
    len = min_t(size_t, len, SIMPLEFS_MAX_FILESIZE - pos);

    /* find extent block */
    struct buffer_head *bh = sb_bread(sb, SIMPLEFS_INODE(inode)->ei_block);
    if (!bh)
        return -EIO;
    struct simplefs_file_ei_block *ei_block =
        (struct simplefs_file_ei_block *) bh->b_data;

    /* count block position */
    sector_t block_index = pos / SIMPLEFS_BLOCK_SIZE;
    sector_t ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;

    /* write data */
    while (len > 0) {
        /* check if block is allocated */
        if (ei_block->extents[ei_index].ee_start == 0) {
            int bno = get_free_blocks(sb, 8);
            if (!bno) {
                bytes_write = -ENOSPC;
                break;
            }
            ei_block->extents[ei_index].ee_start = bno;
            ei_block->extents[ei_index].ee_len = 8;
            ei_block->extents[ei_index].ee_block =
                ei_index ? ei_block->extents[ei_index - 1].ee_block +
                               ei_block->extents[ei_index - 1].ee_len
                         : 0;
        }

        struct buffer_head *bh_data =
            sb_bread(sb, ei_block->extents[ei_index].ee_start +
                             block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
        if (!bh_data) {
            pr_err("Failed to read data block %llu\n",
                   ei_block->extents[ei_index].ee_start +
                       block_index % SIMPLEFS_MAX_BLOCKS_PER_EXTENT);
            bytes_write = -EIO;
            break;
        }
        /* copy data from buffer */
        size_t bytes_to_write =
            min_t(size_t, len, SIMPLEFS_BLOCK_SIZE - pos % SIMPLEFS_BLOCK_SIZE);

        if (copy_from_user(bh_data->b_data + pos % SIMPLEFS_BLOCK_SIZE, buf,
                           bytes_to_write)) {
            brelse(bh_data);
            bytes_write = -EFAULT;
            break;
        }

        mark_buffer_dirty(bh_data);
        sync_dirty_buffer(bh_data);
        brelse(bh_data);

        /* successfully write data */
        len = len - bytes_to_write;
        bytes_write += bytes_to_write;
        pos += bytes_to_write;

        /* count extent block */
        block_index = pos / SIMPLEFS_BLOCK_SIZE;
        ei_index = block_index / SIMPLEFS_MAX_BLOCKS_PER_EXTENT;
    }
    mark_buffer_dirty(bh);
    sync_dirty_buffer(bh);
    brelse(bh);

    inode->i_size = max(pos, inode->i_size);
    inode->i_blocks = DIV_ROUND_UP(inode->i_size, SIMPLEFS_BLOCK_SIZE) + 1;
#if SIMPLEFS_AT_LEAST(6, 7, 0)
    struct timespec64 cur_time = current_time(inode);
    inode_set_mtime_to_ts(inode, cur_time);
    inode_set_ctime_to_ts(inode, cur_time);
#elif SIMPLEFS_AT_LEAST(6, 6, 0)
    struct timespec64 cur_time = current_time(inode);
    inode->i_mtime = cur_time;
    inode_set_ctime_to_ts(inode, cur_time);
#else
    inode->i_mtime = inode->i_ctime = current_time(inode);
#endif
    mark_inode_dirty(inode);
    *ppos = pos;

    return bytes_write;
}

const struct address_space_operations simplefs_aops = {
#if SIMPLEFS_AT_LEAST(5, 19, 0)
    .readahead = simplefs_readahead,
#else
    .readpage = simplefs_readpage,
#endif
    .writepage = simplefs_writepage,
    .write_begin = simplefs_write_begin,
    .write_end = simplefs_write_end,
};

const struct file_operations simplefs_file_ops = {
    .owner = THIS_MODULE,
    .open = simplefs_open,
    .read = simplefs_read,
    .write = simplefs_write,
    .llseek = generic_file_llseek,
    .fsync = generic_file_fsync,
};
