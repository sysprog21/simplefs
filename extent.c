#include <linux/fs.h>
#include <linux/kernel.h>

#include "simplefs.h"

/* Search for the extent containing the target block.
 * Returns the first unused file index if not found.
 * Returns -1 if the target block is out of range.
 * Binary search is used for efficiency.
 */
uint32_t simplefs_ext_search(struct simplefs_file_ei_block *index,
                             uint32_t iblock)
{
    /* first, find the first unused file index with binary search.
     * It'll be our right boundary for actual binary search
     * and return value when file index is not found.
     */
    uint32_t start = 0;
    uint32_t end = SIMPLEFS_MAX_EXTENTS - 1;
    uint32_t boundary;
    uint32_t end_block;
    uint32_t end_len;

    while (start < end) {
        uint32_t mid = start + (end - start) / 2;
        if (index->extents[mid].ee_start == 0) {
            end = mid;
        } else {
            start = mid + 1;
        }
    }

    if (index->extents[end].ee_start == 0) {
        boundary = end;
    } else {
        /* File index full */
        boundary = end + 1;
    }

    if (boundary == 0) {
        /* No used file index */
        return boundary;
    }

    /* try finding target block using binary search */
    start = 0;
    end = boundary - 1;
    while (start < end) {
        uint32_t mid = start + (end - start) / 2;
        uint32_t block = index->extents[mid].ee_block;
        uint32_t len = index->extents[mid].ee_len;
        if (iblock >= block && iblock < block + len) {
            /* found before search finished */
            return mid;
        }
        if (iblock < block) {
            end = mid;
        } else {
            start = mid + 1;
        }
    }

    /* return 'end' if it directs to valid block
     * return 'boundary' if index is not found
     * and eiblock has remaining space */
    end_block = index->extents[end].ee_block;
    end_len = index->extents[end].ee_len;
    if (iblock >= end_block && iblock < end_len) {
        return end;
    } else if (boundary < SIMPLEFS_MAX_EXTENTS) {
        return boundary;
    }
    return boundary;
}