#!/bin/sh
# Builds and runs the perf lab's microbenchmarks (perf_bench.c) as a guest
# image of the native build: the guest's memory functions against musl's
# (checked alike first), skinning, the game's maths. It starts no game.
# Run from the repository's root after python3 configure.py.
set -e
ninja macos_perf_bench
cd build/macos/perf_bench/Halo
./halo 2> /dev/null | grep -v "^halo host"
