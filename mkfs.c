#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "simplefs.h"

struct superblock {
    union {
        struct simplefs_sb_info info;
        char padding[SIMPLEFS_BLOCK_SIZE]; /* Padding to match block size */
    };
};

_Static_assert(sizeof(struct superblock) == SIMPLEFS_BLOCK_SIZE);

/**
 * DIV_ROUND_UP - round up a division
 * @n: dividend
 * @d: divisor
 *
 * Return the result of n / d, rounded up to the nearest integer.
 */
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

static struct superblock *write_superblock(int fd, struct stat *fstats)
{
    struct superblock *sb = malloc(sizeof(struct superblock));
    if (!sb)
        return NULL;

    uint32_t nr_blocks = fstats->st_size / SIMPLEFS_BLOCK_SIZE;
    uint32_t nr_inodes = nr_blocks;
    uint32_t mod = nr_inodes % SIMPLEFS_INODES_PER_BLOCK;
    if (mod)
        nr_inodes += SIMPLEFS_INODES_PER_BLOCK - mod;
    uint32_t nr_istore_blocks =
        DIV_ROUND_UP(nr_inodes, SIMPLEFS_INODES_PER_BLOCK);
    uint32_t nr_ifree_blocks = DIV_ROUND_UP(nr_inodes, SIMPLEFS_BLOCK_SIZE * 8);
    uint32_t nr_bfree_blocks = DIV_ROUND_UP(nr_blocks, SIMPLEFS_BLOCK_SIZE * 8);
    uint32_t nr_data_blocks =
        nr_blocks - nr_istore_blocks - nr_ifree_blocks - nr_bfree_blocks;

    memset(sb, 0, sizeof(struct superblock));
    sb->info = (struct simplefs_sb_info){
        .magic = htole32(SIMPLEFS_MAGIC),
        .nr_blocks = htole32(nr_blocks),
        .nr_inodes = htole32(nr_inodes),
        .nr_istore_blocks = htole32(nr_istore_blocks),
        .nr_ifree_blocks = htole32(nr_ifree_blocks),
        .nr_bfree_blocks = htole32(nr_bfree_blocks),
        .nr_free_inodes = htole32(nr_inodes - 1),
        .nr_free_blocks = htole32(nr_data_blocks - 1),
    };

    int ret = write(fd, sb, sizeof(struct superblock));
    if (ret != sizeof(struct superblock)) {
        free(sb);
        return NULL;
    }

    printf(
        "Superblock: (%ld)\n"
        "\tmagic=%#x\n"
        "\tnr_blocks=%u\n"
        "\tnr_inodes=%u (istore=%u blocks)\n"
        "\tnr_ifree_blocks=%u\n"
        "\tnr_bfree_blocks=%u\n"
        "\tnr_free_inodes=%u\n"
        "\tnr_free_blocks=%u\n",
        sizeof(struct superblock), sb->info.magic, sb->info.nr_blocks,
        sb->info.nr_inodes, sb->info.nr_istore_blocks, sb->info.nr_ifree_blocks,
        sb->info.nr_bfree_blocks, sb->info.nr_free_inodes,
        sb->info.nr_free_blocks);

    return sb;
}

static int write_inode_store(int fd, struct superblock *sb)
{
    /* Allocate a block of zeroed-out memory space for the inode storage. */
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;

    memset(block, 0, SIMPLEFS_BLOCK_SIZE);

    /* Root inode (inode 1) */
    struct simplefs_inode *inode = (struct simplefs_inode *) block;
    uint32_t first_data_block = 1 + le32toh(sb->info.nr_bfree_blocks) +
                                le32toh(sb->info.nr_ifree_blocks) +
                                le32toh(sb->info.nr_istore_blocks);

    /* Designate inode 1 as the root inode.
     * When the system uses the glibc, the readdir function will skip over
     * inode 0. Additionally, the VFS layer avoids using inode 0 to prevent
     * potential issues.
     */
    inode += 1;
    inode->i_mode = htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
                            S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);
    inode->i_uid = 0;
    inode->i_gid = 0;
    inode->i_size = htole32(SIMPLEFS_BLOCK_SIZE);
    inode->i_ctime = inode->i_atime = inode->i_mtime = htole32(0);
    inode->i_blocks = htole32(1);
    inode->i_nlink = htole32(2);
    inode->ei_block = htole32(first_data_block);

    int ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* Clear all memory blocks allocated for inode storage. */
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);
    uint32_t i;
    for (i = 1; i < sb->info.nr_istore_blocks; i++) {
        ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf(
        "Inode store: wrote %d blocks\n"
        "\tinode size = %ld B\n",
        i, sizeof(struct simplefs_inode));

end:
    free(block);
    return ret;
}

static int write_ifree_blocks(int fd, struct superblock *sb)
{
    char *block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;

    uint64_t *ifree = (uint64_t *) block;

    /* Set all bits to 1 */
    memset(ifree, 0xff, SIMPLEFS_BLOCK_SIZE);

    /* The initial ifree block holds the first inode marked as in-use. */
    ifree[0] = htole64(0xfffffffffffffffc);
    int ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* All free blocks in the inode bitmap except the one containing the first
     * two inodes.
     */
    ifree[0] = 0xffffffffffffffff;
    uint32_t i;
    for (i = 1; i < le32toh(sb->info.nr_ifree_blocks); i++) {
        ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf("Ifree blocks: wrote %d blocks\n", i);

end:
    free(block);

    return ret;
}

static int write_bfree_blocks(int fd, struct superblock *sb)
{
    uint32_t nr_used = le32toh(sb->info.nr_istore_blocks) +
                       le32toh(sb->info.nr_ifree_blocks) +
                       le32toh(sb->info.nr_bfree_blocks) + 2;

    char *block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;
    uint64_t *bfree = (uint64_t *) block;

    /* The first blocks refer to the superblock (metadata about the fs), inode
     * store (where inode data is stored), ifree (list of free inodes), bfree
     * (list of free data blocks), and one data block marked as used.
     */
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    uint32_t i = 0;
    while (nr_used) {
        uint64_t line = 0xffffffffffffffff;
        for (uint64_t mask = 0x1; mask; mask <<= 1) {
            line &= ~mask;
            nr_used--;
            if (!nr_used)
                break;
        }
        bfree[i] = htole64(line);
        i++;
    }
    int ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* other blocks */
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    for (i = 1; i < le32toh(sb->info.nr_bfree_blocks); i++) {
        ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
        if (ret != SIMPLEFS_BLOCK_SIZE) {
            ret = -1;
            goto end;
        }
    }
    ret = 0;

    printf("Bfree blocks: wrote %d blocks\n", i);
end:
    free(block);

    return ret;
}

static int write_data_blocks(int fd, struct superblock *sb)
{
    char *buffer = calloc(1, SIMPLEFS_BLOCK_SIZE);
    if (!buffer) {
        perror("Failed to allocate memory");
        return -1;
    }

    ssize_t ret = write(fd, buffer, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        perror("Failed to write data block");
        free(buffer);
        return -1;
    }

    free(buffer);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Open disk image */
    int fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        perror("open():");
        return EXIT_FAILURE;
    }

    /* Get image size */
    struct stat stat_buf;
    int ret = fstat(fd, &stat_buf);
    if (ret) {
        perror("fstat():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Get block device size */
    if ((stat_buf.st_mode & S_IFMT) == S_IFBLK) {
        long int blk_size = 0;
        ret = ioctl(fd, BLKGETSIZE64, &blk_size);
        if (ret != 0) {
            perror("BLKGETSIZE64:");
            ret = EXIT_FAILURE;
            goto fclose;
        }
        stat_buf.st_size = blk_size;
    }

    /* Verify if the file system image has sufficient size. */
    long int min_size = 100 * SIMPLEFS_BLOCK_SIZE;
    if (stat_buf.st_size < min_size) {
        fprintf(stderr, "File is not large enough (size=%ld, min size=%ld)\n",
                stat_buf.st_size, min_size);
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Write superblock (block 0) */
    struct superblock *sb = write_superblock(fd, &stat_buf);
    if (!sb) {
        perror("write_superblock():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Write inode store blocks (from block 1) */
    ret = write_inode_store(fd, sb);
    if (ret) {
        perror("write_inode_store():");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write inode free bitmap blocks */
    ret = write_ifree_blocks(fd, sb);
    if (ret) {
        perror("write_ifree_blocks()");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* Write block free bitmap blocks */
    ret = write_bfree_blocks(fd, sb);
    if (ret) {
        perror("write_bfree_blocks()");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

    /* clear a root index block */
    ret = write_data_blocks(fd, sb);
    if (ret) {
        perror("write_data_blocks():");
        ret = EXIT_FAILURE;
        goto free_sb;
    }

free_sb:
    free(sb);
fclose:
    close(fd);

    return ret;
}
