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
