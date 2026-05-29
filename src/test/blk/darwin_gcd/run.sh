#!/bin/sh
# Standalone functional validator for the macOS darwin_gcd_queue_t BlueStore
# async I/O backend. Compiles the REAL src/blk/kernel/dispatch_io.cc against
# thin stub headers (so no full Ceph build / boost / rocksdb is required) and
# runs it on a live macOS host. See README.md.
#
# Usage:  sh run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
blk="$here/../../../blk"          # ceph/src/blk
sdk=$(xcrun --show-sdk-path)

clang++ -std=c++17 -fblocks -O1 -g -Wall -Wextra -Wno-unused-parameter \
  -I "$here/stubs" \
  -I "$blk/kernel" \
  -isysroot "$sdk" \
  "$blk/kernel/dispatch_io.cc" \
  "$here/test_darwin_gcd_queue.cc" \
  -o "$here/run"

"$here/run"
