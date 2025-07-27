# test create max nr files
test_create_max_nr_files() {
    echo
    echo "max size $MAXFILES"
    for ((i=0; i<=$MAXFILES; i++))
    do
        test_op "touch $i.txt" Failed # expected to fail with more than $MAXFILES files
    done
    sync
    filecnts=$(ls | wc -w)
    test $filecnts -eq $MAXFILES || echo "Failed($filecnts), it should be $MAXFILES files"
}

test_rm_all_files() {
    echo
    echo "remove all files"
    find . -name '[0-9]*.txt' | xargs -n 2000 sudo rm
    sync
}

test_rand_access_files() {
    STARTNR=$1
    INTERVAL=$2
    echo
    echo "remove from $STARTNR, interval: $INTERVAL, Max: $MAXFILES"
    for ((i=$STARTNR; i<$MAXFILES; i+=$INTERVAL))
    do
        test_op "rm $i.txt" Failed
    done
    echo "create from $STARTNR, interval: $INTERVAL, Max: $MAXFILES"
    for ((i=$STARTNR; i<$MAXFILES; i+=$INTERVAL))
    do
        test_op "touch $i"_1".txt" Failed
    done
    echo
}

test_rand_access_files_exist() {
    echo
    echo "quick check files"
    for ((i=0; i<$MAXFILES; i++))
    do
        quick_check_exist $i"_1".txt
    done
    echo
}
