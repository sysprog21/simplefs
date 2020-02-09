#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "simplefs.h"

struct superblock {
    struct simplefs_sb_info info;
    char padding[4064]; /* Padding to match block size */
};

struct simplefs_file_index_block {
    uint32_t blocks[SIMPLEFS_BLOCK_SIZE >> 2];
};

struct simplefs_dir_block {
    struct simplefs_file {
        uint32_t inode;
        char filename[SIMPLEFS_FILENAME_LEN];
    } files[SIMPLEFS_MAX_SUBFILES];
};

/* Returns ceil(a/b) */
static inline uint32_t idiv_ceil(uint32_t a, uint32_t b)
{
    uint32_t ret = a / b;
    if (a % b)
        return ret + 1;
    return ret;
}

static struct superblock *write_superblock(int fd, struct stat *fstats)
{
    int ret;
    struct superblock *sb;
    uint32_t nr_inodes = 0, nr_blocks = 0, nr_ifree_blocks = 0;
    uint32_t nr_bfree_blocks = 0, nr_data_blocks = 0, nr_istore_blocks = 0;
    uint32_t mod;

    sb = malloc(sizeof(struct superblock));
    if (!sb)
        return NULL;

    nr_blocks = fstats->st_size / SIMPLEFS_BLOCK_SIZE;
    nr_inodes = nr_blocks;
    mod = nr_inodes % SIMPLEFS_INODES_PER_BLOCK;
    if (mod)
        nr_inodes += mod;
    nr_istore_blocks = idiv_ceil(nr_inodes, SIMPLEFS_INODES_PER_BLOCK);
    nr_ifree_blocks = idiv_ceil(nr_inodes, SIMPLEFS_BLOCK_SIZE * 8);
    nr_bfree_blocks = idiv_ceil(nr_blocks, SIMPLEFS_BLOCK_SIZE * 8);
    nr_data_blocks =
        nr_blocks - 1 - nr_istore_blocks - nr_ifree_blocks - nr_bfree_blocks;

    memset(sb, 0, sizeof(struct superblock));
    sb->info.magic = htole32(SIMPLEFS_MAGIC);
    sb->info.nr_blocks = htole32(nr_blocks);
    sb->info.nr_inodes = htole32(nr_inodes);
    sb->info.nr_istore_blocks = htole32(nr_istore_blocks);
    sb->info.nr_ifree_blocks = htole32(nr_ifree_blocks);
    sb->info.nr_bfree_blocks = htole32(nr_bfree_blocks);
    sb->info.nr_free_inodes = htole32(nr_inodes - 1);
    sb->info.nr_free_blocks = htole32(nr_data_blocks - 1);

    ret = write(fd, sb, sizeof(struct superblock));
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
    int ret = 0;
    uint32_t i;
    struct simplefs_inode *inode;
    char *block;
    uint32_t first_data_block;

    /* Allocate a zeroed block for inode store */
    block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);

    /* Root inode (inode 0) */
    inode = (struct simplefs_inode *) block;
    first_data_block = 1 + le32toh(sb->info.nr_bfree_blocks) +
                       le32toh(sb->info.nr_ifree_blocks) +
                       le32toh(sb->info.nr_istore_blocks);
    inode->i_mode = htole32(S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH | S_IWUSR |
                            S_IWGRP | S_IXUSR | S_IXGRP | S_IXOTH);
    inode->i_uid = 0;
    inode->i_gid = 0;
    inode->i_size = htole32(SIMPLEFS_BLOCK_SIZE);
    inode->i_ctime = inode->i_atime = inode->i_mtime = htole32(0);
    inode->i_blocks = htole32(1);
    inode->i_nlink = htole32(2);
    inode->index_block = htole32(first_data_block);

    ret = write(fd, block, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* Reset inode store blocks to zero */
    memset(block, 0, SIMPLEFS_BLOCK_SIZE);
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
    int ret = 0;
    uint32_t i;
    char *block;
    uint64_t *ifree;

    block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;
    ifree = (uint64_t *) block;

    /* Set all bits to 1 */
    memset(ifree, 0xff, SIMPLEFS_BLOCK_SIZE);

    /* First ifree block, containing first used inode */
    ifree[0] = htole64(0xfffffffffffffffe);
    ret = write(fd, ifree, SIMPLEFS_BLOCK_SIZE);
    if (ret != SIMPLEFS_BLOCK_SIZE) {
        ret = -1;
        goto end;
    }

    /* All ifree blocks except the one containing 2 first inodes */
    ifree[0] = 0xffffffffffffffff;
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
    int ret = 0;
    uint32_t i;
    char *block;
    uint64_t *bfree, mask, line;
    uint32_t nr_used = le32toh(sb->info.nr_istore_blocks) +
                       le32toh(sb->info.nr_ifree_blocks) +
                       le32toh(sb->info.nr_bfree_blocks) + 2;

    block = malloc(SIMPLEFS_BLOCK_SIZE);
    if (!block)
        return -1;
    bfree = (uint64_t *) block;

    /*
     * First blocks (incl. sb + istore + ifree + bfree + 1 used block)
     * we suppose it won't go further than the first block
     */
    memset(bfree, 0xff, SIMPLEFS_BLOCK_SIZE);
    i = 0;
    while (nr_used) {
        line = 0xffffffffffffffff;
        for (mask = 0x1; mask; mask <<= 1) {
            line &= ~mask;
            nr_used--;
            if (!nr_used)
                break;
        }
        bfree[i] = htole64(line);
        i++;
    }
    ret = write(fd, bfree, SIMPLEFS_BLOCK_SIZE);
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
    /* FIXME: unimplemented */
    return 0;
}

int main(int argc, char **argv)
{
    int ret = EXIT_SUCCESS, fd;
    long int min_size;
    struct stat stat_buf;
    struct superblock *sb = NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s disk\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Open disk image */
    fd = open(argv[1], O_RDWR);
    if (fd == -1) {
        perror("open():");
        return EXIT_FAILURE;
    }

    /* Get image size */
    ret = fstat(fd, &stat_buf);
    if (ret) {
        perror("fstat():");
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Check if image is large enough */
    min_size = 100 * SIMPLEFS_BLOCK_SIZE;
    if (stat_buf.st_size <= min_size) {
        fprintf(stderr, "File is not large enough (size=%ld, min size=%ld)\n",
                stat_buf.st_size, min_size);
        ret = EXIT_FAILURE;
        goto fclose;
    }

    /* Write superblock (block 0) */
    sb = write_superblock(fd, &stat_buf);
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

    /* Write data blocks */
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
