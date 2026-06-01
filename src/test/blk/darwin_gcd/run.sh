#!/bin/sh
# Standalone functional validator for the macOS darwin_gcd_queue_t BlueStore
# async I/O backend. Compiles the REAL src/blk/kernel/dispatch_io.cc against
# thin stub headers (so no full Ceph build / boost / rocksdb is required) and
# runs it on a live macOS host. See README.md.
#
# Three test binaries:
#   run          - happy-path io_queue_t contract  (test_darwin_gcd_queue.cc)
#   run_edge     - error / edge-path branches       (test_darwin_gcd_edge.cc)
#   run_clsname  - cls-plugin name-derivation guard (test_cls_name_filter.cc)
#
# Usage:  sh run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
blk="$here/../../../blk"          # ceph/src/blk
sdk=$(xcrun --show-sdk-path)

CXXFLAGS="-std=c++17 -fblocks -O1 -g -Wall -Wextra -Wno-unused-parameter \
  -I $here/stubs -I $blk/kernel -isysroot $sdk"

# The backend .cc is shared by both io tests; compile it once.
clang++ $CXXFLAGS -c "$blk/kernel/dispatch_io.cc" -o "$here/dispatch_io.o"

clang++ $CXXFLAGS "$here/dispatch_io.o" "$here/test_darwin_gcd_queue.cc" -o "$here/run"
clang++ $CXXFLAGS "$here/dispatch_io.o" "$here/test_darwin_gcd_edge.cc"  -o "$here/run_edge"
# The cls name-filter test is pure string logic (no backend / no -fblocks).
clang++ -std=c++17 -O1 -g -Wall -Wextra -isysroot "$sdk" \
  "$here/test_cls_name_filter.cc" -o "$here/run_clsname"

echo "== happy-path contract test =="
"$here/run"
echo
echo "== edge / error-path test =="
"$here/run_edge"
echo
echo "== cls name-derivation guard =="
"$here/run_clsname"
