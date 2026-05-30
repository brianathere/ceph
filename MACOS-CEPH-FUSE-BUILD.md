# ceph-fuse builds and runs natively on macOS (Apple Silicon)

**Milestone:** `ceph-fuse` — and the entire client stack it links
(common, msg, osdc, osd, mon, mds, client, librados, BlueStore + the
`darwin_gcd_queue_t` backend) — now **compiles and links on macOS arm64**, and
the binary runs:

```
$ ceph-fuse --version
ceph version 20.0.0-11157-g0ee19106 (0ee191069f5a40a4...) tentacle (dev)
```

Binary: Mach-O 64-bit executable arm64, ~49 MB.
Toolchain: macOS 26.5 / Apple clang / Homebrew / vendored Boost (mach-o) /
rocksdb / FUSE 3 headers from fuse-t.

## The macOS port, commit by commit (branch `macos-bluestore-gcd`)

Build-system / toolchain:
- accept AppleClang as the Boost toolset
- build Boost.Context as Mach-O on Apple Silicon (not ELF asm)
- gate `uring::uring` (Linux io_uring) out of cephfs-tool
- `-fblocks` for the GCD backend TU

libc++ / Darwin source portability (each a real compile-breaker on Darwin that
libstdc++/glibc tolerated):
- `error_code.h`: `#include <exception>` (libc++ no transitive include)
- `OSDMap.cc` / `SplitOp.cc` / metrics `Types.h`: `std::max/min`/encode/decode
  pinned to `uint64_t` (size_t != uint64_t as distinct types on Darwin)
- `signal_handler.cc`: `strsignal` instead of glibc `sigdescr_np`
- `admin_socket.cc`: guard Linux POSIX per-process timers (`timer_create`…)
- `compat.cc`: portable secure-zero (no `explicit_bzero`/`memset_s` reliance)
- `compat.h`: full `cpu_set_t`/CPU_*/`sched_*affinity` shim (C++-guarded),
  `PROCPREFIX`, `ENOKEY`
- `numa.h`: pull in the cpu_set_t shim on `__APPLE__`
- `Journald.cc`: `<endian.h>`→OSByteOrder, `sockaddr_un.sun_len`,
  `memfd_create`/`O_TMPFILE` guards, `SOCK_CLOEXEC`, `getprogname`, thread-id
  format (opaque `pthread_t`)
- `tcp_info.cc`: no-op stub (Linux `TCP_INFO` has no portable Darwin analog)
- `crypto_onwire.h`: `<string>`/`<string_view>`
- `pick_address.cc`: `<net/if.h>`
- `ceph_fuse.h`: build against FUSE 3 (fuse-t) headers (the API version it asks
  for) instead of fuse2 compat headers

Plus the headline feature this branch exists for: **`darwin_gcd_queue_t`**, the
idiomatic macOS BlueStore async I/O backend (libdispatch worker pool + kqueue
`EVFILT_USER` completion doorbell + `F_FULLFSYNC` durability), with a standalone
validator under `src/test/blk/darwin_gcd`.

## Build recipe (summary)

Prereqs: `brew install cmake ninja icu4c@78 openssl@3 nss snappy pkg-config`;
a uv venv with `cython pyyaml prettytable`; FUSE 3 headers+lib (fuse-t);
submodules `rocksdb zstd xxHash fmt c-ares utf8proc BLAKE3
erasure-code/jerasure` inited.

```sh
cmake -G Ninja .. -DCMAKE_BUILD_TYPE=Release \
  -DWITH_BLUESTORE=ON -DWITH_CEPHFS=ON -DWITH_LIBCEPHFS=ON -DWITH_FUSE=ON \
  -DWITH_SYSTEM_BOOST=OFF -DWITH_MGR=OFF -DWITH_RBD=OFF -DWITH_RADOSGW=OFF \
  -DWITH_LIBURING=OFF -DWITH_SPDK=OFF -DWITH_XFS=OFF -DWITH_RDMA=OFF \
  -DWITH_BREAKPAD=OFF -DWITH_MANPAGE=OFF -DWITH_TESTS=OFF -DENABLE_GIT_VERSION=OFF \
  -DPython3_EXECUTABLE=<venv>/bin/python \
  -DCMAKE_PREFIX_PATH="$(brew --prefix icu4c@78);$(brew --prefix openssl@3);$(brew --prefix nss);$(brew --prefix)" \
  -DFUSE_INCLUDE_DIR=<fuse3-prefix>/include/fuse3 \
  -DFUSE_LIBRARIES=<fuse3-prefix>/lib/libfuse3.dylib
ninja ceph-fuse ceph-osd ceph-mon ceph-mds ceph rados
```

## Running against 4 hdiutil-backed BlueStore OSDs

4 raw devices via `hdiutil create -type SPARSE` + `hdiutil attach -nomount`
(`/dev/disk6..9`, 2 GiB each) back 4 BlueStore OSDs (which selects
`darwin_gcd_queue_t` via `bdev_dispatchio` default-on for APPLE). Cluster brought
up with `vstart.sh -n -X -l --bluestore --bluestore-devs /dev/disk6,7,8,9`,
`ceph fs new`, then `ceph-fuse /mnt`.

The one runtime gate for the *mount* itself: fuse-t's userspace NFS server (or
macFUSE) must be installed via its `.pkg`, which needs `sudo`. The cluster + OSD
data path on the 4 disks exercises the new backend regardless.
