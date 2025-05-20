# file too large
test_too_large_file() {
    test_op 'dd if=/dev/zero of=file bs=1M count=12 status=none'
    filesize=$(sudo ls -lR  | grep -e "$F_MOD 2".*file | awk '{print $5}')
    test $filesize -le $MAXFILESIZE || echo "Failed, file size over the limit"
}

# Write the file size larger than BLOCK_SIZE
# test serial to write
test_file_size_larger_than_block_size() {
    test_op 'yes 123456789 | head -n 1600 | tr -d "\n" > file.txt'
    count=$(awk '{count += gsub(/123456789/, "")} END {print count}' "file.txt")
    echo "test $count"
    test "$count" -eq 1600 || echo "Failed, file size not matching"
    # test block to write
    test_op 'cat file.txt > checkfile.txt'
    count=$(awk '{count += gsub(/123456789/, "")} END {print count}' "checkfile.txt")
    echo "test $count"
    test "$count" -eq 1600 || echo "Failed, file size not matching"
}
