test_op() {
    local op=$1
    local option=${2:-Success}

    if [ "$option" = "Success" ]; then
        sudo sh -c "$op" >/dev/null && echo  "Testing cmd: $op... Success"
    elif [ "$option" = "Failed" ]; then
        sudo sh -c "$op" >/dev/null || echo  "Testing cmd: $op... Failed"
    fi
}

check_exist() {
    local mode=$1
    local nlink=$2
    local name=$3
    sudo ls -lR  | grep -e "$mode $nlink".*$name >/dev/null || echo "Failed"
}

quick_check_exist() {
    local name=$1
    [ ! -f $name ] && echo "Not found: $name"
}
