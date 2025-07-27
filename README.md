# simplefs - a simple file system for Linux

The file system "simplefs" serves as a helpful tool for understanding the basics
of the Linux Virtual File System (VFS) and fs basics. Linux VFS accommodates
multiple file systems, with the kernel handling the bulk of the operations while
delegating file system-specific tasks to individual file systems via handlers.
Instead of directly invoking functions, the kernel employs various operation
tables. These tables are collections of handlers for each operation, essentially
structures comprised of function pointers for handlers/callbacks.

Super block operations are established at the time of mounting. The operation
tables for inodes and files are set when the inode is accessed. The initial step
before accessing an inode involves a lookup process. The inode for a file is
identified by invoking the lookup handler of the parent inode.

## Features

* Directories: create, remove, list, rename;
* Regular files: create, remove, read/write (through page cache), rename;
* Hard/Symbolic links (also symlink or soft link): create, remove, rename;
* No extended attribute support

## Prerequisites

Install linux kernel header in advance.
```shell
$ sudo apt install linux-headers-$(uname -r)
```

## Build and Run

You can build the kernel module and tool with `make`.
Generate test image via `make test.img`, which creates a zeroed file of 50 MiB.

You can then mount this image on a system with the simplefs kernel module installed.
Let's test kernel module:
```shell
$ sudo insmod simplefs.ko
```

Corresponding kernel message:
```
simplefs: module loaded
```

Generate test image by creating a zeroed file of 50 MiB. We can then mount
this image on a system with the simplefs kernel module installed.
```shell
$ mkdir -p test
$ dd if=/dev/zero of=test.img bs=1M count=50
$ ./mkfs.simplefs test.img
$ sudo mount -o loop -t simplefs test.img test
```

You shall get the following kernel messages:
```
simplefs: '/dev/loop?' mount success
```
Here `/dev/loop?` might be `loop1`, `loop2`, `loop3`, etc.

Perform regular file system operations: (as root)
```shell
$ echo "Hello World" > test/hello
$ cat test/hello
$ ls -lR
```

Remove kernel mount point and module:
```shell
$ sudo umount test
$ sudo rmmod simplefs
```

## Design

At present, simplefs only provides straightforward features.

### Partition layout
```
+------------+-------------+-------------------+-------------------+-------------+
| superblock | inode store | inode free bitmap | block free bitmap | data blocks |
+------------+-------------+-------------------+-------------------+-------------+
```
Each block is 4 KiB large.

### Superblock
The superblock, located at the first block of the partition (block 0), stores
the partition's metadata. This includes the total number of blocks, the total
number of inodes, and the counts of free inodes and blocks.

### Inode store
This section contains all the inodes of the partition, with the maximum number
of inodes being equal to the number of blocks in the partition. Each inode
occupies 72 bytes of data, encompassing standard information such as the file
size and the number of blocks used, in addition to a simplefs-specific field
named `ei_block`. This field, `ei_block`, serves different purposes depending
on the type of file:
  - For a directory, it contains the list of files within that directory.
    A directory can hold a maximum of 40,920 files, with filenames restricted
    to a maximum of 255 characters to ensure they fit within a single block.
  ```
  inode
  +-----------------------+
  | i_mode = IFDIR | 0755 |      block 123 (simplefs_file_ei_block)
  | ei_block = 123    ----|--->  +----------------+
  | i_size = 4 KiB        |      | nr_files  = 7  |
  | i_blocks = 1          |      |----------------|
  +-----------------------+    0 | ee_block  = 0  |
                                 | ee_len    = 8  |      block 84(simplefs_dir_block)
                                 | ee_start  = 84 |--->  +-------------+
                                 | nr_file   = 2  |      |nr_files = 2 |
                                 |----------------|      |-------------|
                               1 | ee_block  = 8  |    0 | inode  = 24 |
                                 | ee_len    = 8  |      | nr_blk = 1  |
                                 | ee_start  = 16 |      | (foo)       |
                                 | nr_file   = 5  |      |-------------|
                                 |----------------|    1 | inode  = 45 |
                                 | ...            |      | nr_blk = 14 |
                                 |----------------|      | (bar)       |
                             341 | ee_block  = 0  |      |-------------|
                                 | ee_len    = 0  |      | ...         |
                                 | ee_start  = 0  |      |-------------|
                                 | nr_file   = 12 |   14 | 0           |
                                 +----------------+      +-------------+

  ```
  - For a file, it lists the extents that hold the actual data of the file.
    Given that block IDs are stored as values of `sizeof(struct simplefs_extent)`
    bytes, a single block can accommodate up to 341 links. This limitation
    restricts the maximum size of a file to approximately 10.65 MiB (10,912 KiB).
  ```
  inode
  +-----------------------+
  | i_mode = IFDIR | 0644 |          block 93
  | ei_block = 93     ----|------>  +----------------+
  | i_size = 10 KiB       |       0 | ee_block  = 0  |
  | i_blocks = 25         |         | ee_len    = 8  |      extent 94
  +-----------------------+         | ee_start  = 94 |---> +--------+
                                    |----------------|     |        |
                                  1 | ee_block  = 8  |     +--------+
                                    | ee_len    = 8  |      extent 99
                                    | ee_start  = 99 |---> +--------+
                                    |----------------|     |        |
                                  2 | ee_block  = 16 |     +--------+
                                    | ee_len    = 8  |      extent 66
                                    | ee_start  = 66 |---> +--------+
                                    |----------------|     |        |
                                    | ...            |     +--------+
                                    |----------------|
                                341 | ee_block  = 0  |
                                    | ee_len    = 0  |
                                    | ee_start  = 0  |
                                    +----------------+
  ```

### Extent support
An extent spans consecutive blocks; therefore, we allocate consecutive disk blocks
for it in a single operation. It is defined by `struct simplefs_extent`, which
comprises three members:
- `ee_block`: the first logical block that the extent covers.
- `ee_len`: the number of blocks the extent covers.
- `ee_start`: the first physical block that the extent covers."

```
struct simplefs_extent
  +----------------+
  | ee_block =  0  |
  | ee_len   =  200|              extent
  | ee_start =  12 |-----------> +---------+
  +----------------+    block 12 |         |
                                 +---------+
                              13 |         |
                                 +---------+
                                 | ...     |
                                 +---------+
                             211 |         |
                                 +---------+
```

### journalling support

Simplefs now includes support for an external journal device, leveraging the journaling block device (jbd2) subsystem in the Linux kernel. This enhancement improves the file system's resilience by maintaining a log of changes, which helps prevent corruption and facilitates recovery in the event of a crash or power failure.


The journaling support in simplefs is implemented using the jbd2 subsystem, which is a widely-used journaling layer in Linux. Currently, simplefs primarily stores the journal-related information in an external journal device.

For a detailed introduction to journaling, please refer to these two websites:
[Journal(jbd2) document](https://www.kernel.org/doc/html/latest/filesystems/ext4/journal.html)
[Journal(jbd2) api](https://docs.kernel.org/filesystems/journalling.html)

External journal device disk layout:

+--------------------+------------------+---------------------------+--------------+
| Journal Superblock | Descriptor Block | Metadata/Data ( modified ) | Commit Block |
+--------------------+------------------+---------------------------+--------------+

Hint:
Each transaction starts with a descriptor block, followed by several metadata blocks or data blocks, and ends with a commit block. Every modified metadata (such as inode, bitmap, etc.) occupies its own block. Currently, simplefs primarily records "extent" metadata.


How to Enable Journaling in simplefs:

Step 1: Create the Journal Disk Image
To create an 8MB disk image for the journal, use the following make command:

Note:
Assuming an 8 MB size for the external journal device, which is an arbitrary choice for now, I will set the journal block length to a fixed 2048, calculated by dividing the device size by the block size (4096 bytes).

```shell
$ make journal
```

Step 2:  Make sure you've loaded the SimpleFS Kernel Module

```shell
$ insmod simplefs/simplefs.ko
```

Step 3: Setup the Loop Device for the Journal
Find an available loop device and associate it with the journal image:

``` shell
$ loop_device=$(losetup -f)
$ losetup $loop_device /simplefs/journal.img
```

You shall get the following kernel messages:
```
loop0: detected capacity change from 0 to 16384
```

Step 4: Mount the SimpleFS File System with the External Journal
Mount the SimpleFS file system along with the external journal device using the following command:

```shell
mount -o loop,rw,owner,group,users,journal_path="$loop_device" -t simplefs /simplefs/test.img /test
```

Corresponding kernel message:
```
loop1: detected capacity change from 0 to 409600
simplefs: simplefs_parse_options: parsing options 'owner,group,journal_path=/dev/loop0'
simplefs: '/dev/loop1' mount success
```

Current Limitations and Known Issues

1. External Journal Device Size:

- The exact size of the external journal device cannot be determined. As a temporary solution, the size is set by dividing the device size by the block size, with the external journal device size fixed at 8 MB.

2. Metadata Recording:

- At present, only "extent" metadata is recorded. In the future, additional metadata such as "super block" and inode metadata can be included.

3. Implementation of External Journal Device:

- Only the external journal device is implemented. Future improvements can include the use of an internal journal (inode journal). However, this will require the addition of a bmap function and appropriate adjustments to the disk partition during mkfs.

## License

`simplefs` is released under the BSD 2 clause license. Use of this source code
is governed by a BSD-style license that can be found in the LICENSE file.
