#include <linux/dcache.h>
#include <linux/types.h>
#include "simplefs.h"

uint32_t simplefs_hash(struct dentry *dentry)
{
    const char *str = dentry->d_name.name;
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*str) {
        h ^= (unsigned char) (*str++);
        h *= 0x100000001b3ULL;
    }
    /* Fold high 32 bits into low 32 bits to mix the full 64-bit result */
    return (uint32_t) (h ^ (h >> 32));
}
