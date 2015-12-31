#!/usr/bin/env sh

if [ "$TRAVIS_OS_NAME" = "linux" ]; then
    export CXX=g++-4.8 CC=gcc-4.8
elif [ "$TRAVIS_OS_NAME" = "osx" ]; then
    export CXX=clang++ CC=clang
fi

export CMAKE_CXX_FLAGS=-Werror
export MAKEFLAGS=-j2
