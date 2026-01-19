#!/usr/bin/env bash

set -e

IMAGE=test.img
IMAGESIZE=50
MKFS=mkfs.simplefs

function build_mkfs()
{
    make $MKFS
}

function test_mkfs()
{
    dd if=/dev/zero of=$IMAGE bs=1M count=$IMAGESIZE 2>/dev/null
    ./$MKFS $IMAGE
}

build_mkfs
test_mkfs
