# file too large
test_too_large_file() {
    TESTLG_FILE_SZ=$(( $MAXFILESIZE / 1024 / 1024 + 1 ))
    test_op "dd if=/dev/zero of=exceed_max_sz_file bs=1M count=$TESTLG_FILE_SZ status=none"
    echo
    filesize=$(sudo ls -lR  | grep -e "$F_MOD 1".*file | awk '{print $5}')
    test $filesize -le $MAXFILESIZE || echo "Failed, file size over the limit"
    test_op 'rm exceed_max_sz_file'
    echo
}

# Write the file size larger than BLOCK_SIZE
# test serial to write
test_file_size_larger_than_block_size() {
    test_op 'yes 123456789 | head -n 1600 | tr -d "\n" > exceed_blk.txt'
    echo
    count=$(awk '{count += gsub(/123456789/, "")} END {print count}' "exceed_blk.txt")
    echo "test $count"
    test "$count" -eq 1600 || echo "Failed, file size not matching"
    # test block to write
    test_op 'cat exceed_blk.txt > checkfile.txt'
    echo
    count=$(awk '{count += gsub(/123456789/, "")} END {print count}' "checkfile.txt")
    echo "test $count"
    test "$count" -eq 1600 || echo "Failed, file size not matching"
    test_op 'rm exceed_blk.txt checkfile.txt'
    echo
}
