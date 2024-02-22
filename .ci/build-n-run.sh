#!/usr/bin/env bash

function build_mod()
{   
    make all || exit 1
}

function run_tests()
{
    make check || exit 2
}

build_mod
run_tests
