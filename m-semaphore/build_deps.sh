#!/bin/bash

# Fetch the dependencies
git submodule update --init deps/pcm

# Build PCM
pushd deps/pcm
patch -p1 -N < ../../pcm.patch
mkdir build
cd build
cmake ../
make PCM_STATIC
popd
