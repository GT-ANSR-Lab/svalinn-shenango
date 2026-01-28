#!/bin/bash

# Fetch the dependencies
git submodule update --init ../deps/pcm ../deps/benchmark

# Build PCM
pushd ../deps/pcm
patch -p1 -N < ../../pcm.patch
mkdir -p build
cd build
cmake ../
make PCM_STATIC
popd

# Build Benchmark
pushd ../deps/benchmark
cmake -E make_directory "build"
cmake -E chdir "build" cmake -DBENCHMARK_DOWNLOAD_DEPENDENCIES=on -DCMAKE_BUILD_TYPE=Release ../
cmake --build "build" --config Release
popd
