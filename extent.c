#include <linux/fs.h>
#include <linux/kernel.h>

#include "simplefs.h"

/* Search for the extent containing the target block.
 * Returns the first unused file index if not found.
 * Returns -1 if the target block is out of range.
 * TODO: Implement binary search for efficiency.
 */
uint32_t simplefs_ext_search(struct simplefs_file_ei_block *index,
                             uint32_t iblock)
{
    uint32_t i;
    for (i = 0; i < SIMPLEFS_MAX_EXTENTS; i++) {
        uint32_t block = index->extents[i].ee_block;
        uint32_t len = index->extents[i].ee_len;
        if (index->extents[i].ee_start == 0 ||
            (iblock >= block && iblock < block + len))
            return i;
    }

    return -1;
}
