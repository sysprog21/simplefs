# create 100 files with filenames inside
test_remount_file_exist() {
    for ((i=1; i<=$MOUNT_TEST; i++))
    do
        echo file_$i | sudo tee file_$i.txt >/dev/null && echo "file_$i.txt created."
    done
    sync

    # unmount and remount the filesystem
    echo "Unmounting filesystem..."
    popd >/dev/null || { echo "popd failed"; exit 1; }
    sudo umount test || { echo "umount failed"; exit 1; }
    sleep 1
    echo "Remounting filesystem..."
    sudo mount -t simplefs -o loop $IMAGE test || { echo "mount failed"; exit 1; }
    echo "Remount succeeds."
    pushd test >/dev/null || { echo "pushd failed"; exit 1; }

    # check if files exist and content is correct after remounting
    for ((i=1; i<=$MOUNT_TEST; i++))
    do
        if [[ -f "file_$i.txt" ]]; then
            content=$(cat "file_$i.txt" | tr -d '\000')
            if [[ "$content" == "file_$i" ]]; then
                echo "Success: file_$i.txt content is correct."
            else
                echo "Failed: file_$i.txt content is incorrect."
                exit 1
            fi
        else
            echo "Failed: file_$i.txt does not exist."
            exit 1
        fi
    done
    find . -name 'file_[0-9]*.txt' | xargs sudo rm || { echo "Failed to delete files"; exit 1; }
    sync
}
