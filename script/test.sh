#!/usr/bin/env bash

. script/config
. script/test_func.sh
. script/test_large_file.sh
. script/test_remount.sh
. script/rand_rm_and_create.sh

SIMPLEFS_MOD=simplefs.ko
IMAGE=$1
IMAGESIZE=$2
MKFS=$3

if [ "$EUID" -eq 0 ]
  then echo "Don't run this script as root"
  exit
fi

mkdir -p test
sudo umount test 2>/dev/null
sleep 1
sudo rmmod simplefs 2>/dev/null
sleep 1
(modinfo $SIMPLEFS_MOD || exit 1) && \
echo && \
sudo insmod $SIMPLEFS_MOD  && \
dd if=/dev/zero of=$IMAGE bs=1M count=$IMAGESIZE status=none && \
./$MKFS $IMAGE && \

sudo mount -t simplefs -o loop $IMAGE test && \
pushd test >/dev/null

# test serial to write
test_create_max_nr_files

test_rand_access_files 0 2
sync
test_rand_access_files 1 2
sync
test_rand_access_files_exist
sync
test_rm_all_files

# test remount file exist or not
test_remount_file_exist

popd >/dev/null || { echo "popd failed"; exit 1; }

# Get ready to count free block
sudo touch test/test.txt
sudo rm test/* -rf
sync

nr_free_blk=$(($(dd if=$IMAGE bs=1 skip=28 count=4 2>/dev/null | hexdump -v -e '1/4 "0x%08x\n"')))
echo "$nr_free_blk"
pushd test >/dev/null || { echo "pushd failed"; exit 1; }

# write a file larger than max size
test_too_large_file

# Write the a file larger than BLOCK_SIZE
test_file_size_larger_than_block_size

# mkdir
test_op 'mkdir dir'
test_op 'mkdir dir' # expected to fail

# create file
test_op 'touch file'

# hard link
test_op 'ln file hdlink'
test_op 'mkdir dir/dir'

# symbolic link
test_op 'ln -s file symlink'

# list directory contents
test_op 'ls -lR'

# now it supports longer filename
test_op 'mkdir len_of_name_of_this_dir_is_29'
test_op 'touch len_of_name_of_the_file_is_29'
test_op 'ln -s dir len_of_name_of_the_link_is_29'

# write to file
test_op 'echo abc > file'
test $(cat file) = "abc" || echo "Failed to write"

# test remove symbolic link
test_op 'ln -s file symlink_fake'
test_op 'rm -f symlink_fake'
test_op 'touch symlink_fake'
test_op 'ln file symlink_hard_fake'
test_op 'rm -f symlink_hard_fake'
test_op 'touch symlink_hard_fake'

# test if exist
check_exist $D_MOD 3 dir
check_exist $F_MOD 2 file
check_exist $F_MOD 2 hdlink
check_exist $D_MOD 2 dir
check_exist $S_MOD 1 symlink
check_exist $F_MOD 1 symlink_fake
check_exist $F_MOD 1 symlink_hard_fake

# clean all files and directories
test_op 'rm -rf ./*'

sleep 1
popd >/dev/null
sudo umount test
sudo rmmod simplefs

af_nr_free_blk=$(($(dd if=$IMAGE bs=1 skip=28 count=4 2>/dev/null | hexdump -v -e '1/4 "0x%08x\n"')))
test $nr_free_blk -eq $af_nr_free_blk || echo "Failed, some blocks are not be reclaimed"
