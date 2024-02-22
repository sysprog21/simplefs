#!/usr/bin/env bash

function build_mod()
{   
    make all || exit 1
}

function run_tests()
{
    make check >/tmp/simplefs-out || (cat /tmp/simplefs-out ; exit 2)
}

build_mod
run_tests
