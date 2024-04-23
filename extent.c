#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/sort.h>

#include "simplefs.h"

/* Compare function for sort */
int cmp(const void *a, const void *b)
{
    struct simplefs_extent *extentA = (struct simplefs_extent *)a;
    struct simplefs_extent *extentB = (struct simplefs_extent *)b;
    return extentA->ee_block - extentB->ee_block;
}

/* Binary Search for the extent containing the target block.
 * Returns the first unused file index if not found.
 * Returns -1 if the target block is out of range.
 */
uint32_t simplefs_ext_search(struct simplefs_file_ei_block *index,
                             uint32_t iblock)
{
    /* Sort the extents by ee_block */
    sort(index->extents, SIMPLEFS_MAX_EXTENTS, sizeof(struct simplefs_extent), cmp, NULL);

    uint32_t start = 0;
    uint32_t end = SIMPLEFS_MAX_EXTENTS - 1;
    while (start <= end) {
        uint32_t mid = start + (end - start) / 2;
        uint32_t block = index->extents[mid].ee_block;
        uint32_t len = index->extents[mid].ee_len;
        if (index->extents[mid].ee_start == 0 ||
            (iblock >= block && iblock < block + len))
            return mid;
        if (index->extents[mid].ee_block < iblock)
            start = mid + 1;
        else
            end = mid - 1;
    }

    return -1;
}
