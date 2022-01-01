#!/usr/bin/env bash

SIMPLEFS_MOD=simplefs.ko
IMAGE=$1
IMAGESIZE=$2
MKFS=$3

D_MOD="drwxr-xr-x"
F_MOD="-rw-r--r--"
S_MOD="lrwxrwxrwx"
MAXFILESIZE=11173888 # SIMPLEFS_MAX_EXTENTS * SIMPLEFS_MAX_BLOCKS_PER_EXTENT * SIMPLEFS_BLOCK_SIZE
MAXFILES=40920        # max files per dir

test_op() {
    local op=$1 
    echo
    echo -n "Testing cmd: $op..."
    sudo sh -c "$op" >/dev/null && echo "Success"
}

check_exist() {
    local mode=$1
    local nlink=$2
    local name=$3
    echo
    echo -n "Check if exist: $mode $nlink $name..."
    sudo ls -lR  | grep -e "$mode $nlink".*$name >/dev/null && echo "Success" || \
    echo "Failed" 
}

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

# mkdir
test_op 'mkdir dir'
test_op 'mkdir dir' # expected to fail

# create file
test_op 'touch file'

# create 40920 files
for ((i=0; i<=$MAXFILES; i++))
do
    test_op "touch $i.txt" # expected to fail with more than 40920 files
done
filecnts=$(ls | wc -w)
test $filecnts -eq $MAXFILES || echo "Failed, it should be $MAXFILES files"
find . -name '[0-9]*.txt' | xargs -n 2000 sudo rm

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

# file too large
test_op 'dd if=/dev/zero of=file bs=1M count=12 status=none'
filesize=$(sudo ls -lR  | grep -e "$F_MOD 2".*file | awk '{print $5}')
test $filesize -le $MAXFILESIZE || echo "Failed, file size over the limit"

# test if exist
check_exist $D_MOD 3 dir 
check_exist $F_MOD 2 file
check_exist $F_MOD 2 hdlink
check_exist $D_MOD 2 dir
check_exist $S_MOD 1 symlink 

sleep 1
popd >/dev/null
sudo umount test
sudo rmmod simplefs
