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
  | i_mode = IFDIR | 0755 |            block 123
  | ei_block = 123    ----|-------->  +----------------+
  | i_size = 4 KiB        |         0 | ee_block  = 0  |
  | i_blocks = 1          |           | ee_len    = 8  |      block 84
  +-----------------------+           | ee_start  = 84 |--->  +-----------+
                                      |----------------|    0 | 24 (foo)  |
                                    1 | ee_block  = 8  |      |-----------|
                                      | ee_len    = 8  |    1 | 45 (bar)  |
                                      | ee_start  = 16 |      |-----------|
                                      |----------------|      | ...       |
                                      | ...            |      |-----------|
                                      |----------------|   14 | 0         |
                                  341 | ee_block  = 0  |      +-----------+
                                      | ee_len    = 0  |
                                      | ee_start  = 0  |
                                      +----------------+

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

## TODO

- journalling support

## License

`simplefs` is released under the BSD 2 clause license. Use of this source code
is governed by a BSD-style license that can be found in the LICENSE file.
