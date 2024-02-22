#define pr_fmt(fmt) "simplefs: " fmt

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "simplefs.h"

/* Iterate over the files contained in dir and commit them to @ctx.
 * This function is called by the VFS as ctx->pos changes.
 * Returns 0 on success.
 */
static int simplefs_iterate(struct file *dir, struct dir_context *ctx)
{
    struct inode *inode = file_inode(dir);
    struct simplefs_inode_info *ci = SIMPLEFS_INODE(inode);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh = NULL, *bh2 = NULL;
    struct simplefs_file_ei_block *eblock = NULL;
    struct simplefs_dir_block *dblock = NULL;
    struct simplefs_file *f = NULL;
    int ei = 0, bi = 0, fi = 0;
    int ret = 0;

    /* Check that dir is a directory */
    if (!S_ISDIR(inode->i_mode))
        return -ENOTDIR;

    /* Check that ctx->pos is not bigger than what we can handle (including
     * . and ..)
     */
    if (ctx->pos > SIMPLEFS_MAX_SUBFILES + 2)
        return 0;

    /* Commit . and .. to ctx */
    if (!dir_emit_dots(dir, ctx))
        return 0;

    /* Read the directory index block on disk */
    bh = sb_bread(sb, ci->ei_block);
    if (!bh)
        return -EIO;
    eblock = (struct simplefs_file_ei_block *) bh->b_data;

    ei = (ctx->pos - 2) / SIMPLEFS_FILES_PER_EXT;
    bi = (ctx->pos - 2) % SIMPLEFS_FILES_PER_EXT / SIMPLEFS_FILES_PER_BLOCK;
    fi = (ctx->pos - 2) % SIMPLEFS_FILES_PER_BLOCK;

    /* Iterate over the index block and commit subfiles */
    for (; ei < SIMPLEFS_MAX_EXTENTS; ei++) {
        if (eblock->extents[ei].ee_start == 0)
            break;

        /* Iterate over blocks in one extent */
        for (; bi < eblock->extents[ei].ee_len; bi++) {
            bh2 = sb_bread(sb, eblock->extents[ei].ee_start + bi);
            if (!bh2) {
                ret = -EIO;
                goto release_bh;
            }
            dblock = (struct simplefs_dir_block *) bh2->b_data;
            if (dblock->files[0].inode == 0)
                break;

            /* Iterate every file in one block */
            for (; fi < SIMPLEFS_FILES_PER_BLOCK; fi++) {
                f = &dblock->files[fi];
                if (f->inode &&
                    !dir_emit(ctx, f->filename, SIMPLEFS_FILENAME_LEN, f->inode,
                              DT_UNKNOWN))
                    break;
                ctx->pos++;
            }
            brelse(bh2);
            bh2 = NULL;
        }
    }

release_bh:
    brelse(bh);

    return ret;
}

const struct file_operations simplefs_dir_ops = {
    .owner = THIS_MODULE,
    .iterate_shared = simplefs_iterate,
};
