#ifndef SIMPLEFS_BITMAP_H
#define SIMPLEFS_BITMAP_H

#include <linux/bitmap.h>

#include "simplefs.h"

/* Returns the first bit found and clears the following 'len' consecutive
 * free bits (sets them to 1) in a given in-memory bitmap spanning multiple
 * blocks. Returns 0 if an adequate number of free bits were not found.
 * Assumes the first bit is never free (reserved for the superblock and the
 * root inode), allowing the use of 0 as an error value.
 */
static inline uint32_t get_first_free_bits(unsigned long *freemap,
                                           unsigned long size,
                                           uint32_t len)
{
    uint32_t bit, prev = 0, count = 0;
    for_each_set_bit (bit, freemap, size) {
        if (prev != bit - 1)
            count = 0;
        prev = bit;
        if (++count == len) {
            bitmap_clear(freemap, bit - len + 1, len);
            return bit - len + 1;
        }
    }
    return 0;
}

/* Return an unused inode number and mark it used.
 * Return 0 if no free inode was found.
 */
static inline uint32_t get_free_inode(struct simplefs_sb_info *sbi)
{
    uint32_t ret = get_first_free_bits(sbi->ifree_bitmap, sbi->nr_inodes, 1);
    if (ret)
        sbi->nr_free_inodes--;
    return ret;
}

/* Return 'len' unused block(s) number and mark it used.
 * Return 0 if no enough free block(s) were found.
 */
static inline uint32_t get_free_blocks(struct simplefs_sb_info *sbi,
                                       uint32_t len)
{
    uint32_t ret = get_first_free_bits(sbi->bfree_bitmap, sbi->nr_blocks, len);
    if (ret)
        sbi->nr_free_blocks -= len;
    return ret;
}

/* Mark the 'len' bit(s) from i-th bit in freemap as free (i.e. 1) */
static inline int put_free_bits(unsigned long *freemap,
                                unsigned long size,
                                uint32_t i,
                                uint32_t len)
{
    /* i is greater than freemap size */
    if (i + len - 1 > size)
        return -1;

    bitmap_set(freemap, i, len);

    return 0;
}

/* Mark an inode as unused */
static inline void put_inode(struct simplefs_sb_info *sbi, uint32_t ino)
{
    if (put_free_bits(sbi->ifree_bitmap, sbi->nr_inodes, ino, 1))
        return;

    sbi->nr_free_inodes++;
}

/* Mark len block(s) as unused */
static inline void put_blocks(struct simplefs_sb_info *sbi,
                              uint32_t bno,
                              uint32_t len)
{
    if (put_free_bits(sbi->bfree_bitmap, sbi->nr_blocks, bno, len))
        return;

    sbi->nr_free_blocks += len;
}

#endif /* SIMPLEFS_BITMAP_H */
