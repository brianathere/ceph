# Full native Ceph stack on macOS (Apple Silicon) — BUILT & RUNNING

Branch `macos-bluestore-gcd`. The complete Ceph client + daemon stack now
compiles, links, and runs natively on macOS 26.5 / arm64 / Apple clang, built
against vendored Boost (Mach-O), rocksdb, and FUSE 3 (fuse-t).

## Binaries (all built, all run `--version`)

ceph-fuse · ceph-osd · ceph-mon · ceph-mds · rados · ceph-authtool ·
monmaptool · crushtool · ceph-conf · ceph (CLI)

`ninja [496/496], exit 0, 0 errors.`

This includes **BlueStore** and the headline contribution of this branch, the
**`darwin_gcd_queue_t`** async I/O backend (libdispatch worker pool + kqueue
`EVFILT_USER` completion doorbell + `F_FULLFSYNC` durability), compiled into
`ceph-osd` (`src/blk/.../dispatch_io.cc.o`).

## What the macOS port took (~30 commits)

Build system / toolchain:
- AppleClang Boost toolset; Boost.Context as Mach-O (not ELF) on arm64
- `-Wl,-undefined,dynamic_lookup` for dlopen'd plugins (snappy/lz4)
- global `-isystem openssl@3/include`; link `iconv` + `Boost::locale` into ceph-fuse
- gate Linux-only plugins on macOS: zlib-compressor (isa-l), extblkdev fcm (blkid)
- FUSE 3 (fuse-t) headers; `FUSE_*_VERSION`; ENOKEY; xattr position arg gated to macFUSE
- `-fblocks` for the GCD backend; submodules: rocksdb/zstd/xxHash/fmt/c-ares/
  utf8proc/BLAKE3/jerasure
- env: uv venv with cython + pyyaml + prettytable

Source portability (libc++ / Darwin), each a real compile/link breaker:
- `<exception>`, `<string>`/`<string_view>`, `<net/if.h>`, `<cerrno>` (not
  `<asm-generic/errno-base.h>`) transitive-include gaps
- `std::max/min/p2aligned/p2roundup` `uint64_t`-vs-`size_t` deduction casts
  (OSDMap, SplitOp, AllocatorBase, ECUtil, ECExtentCache, ReplicatedBackend, BlueFS)
- endian helpers (`htole*`/`le*toh`) via `<libkern/OSByteOrder.h>`
- `_DARWIN_C_SOURCE` before includes for `preadv`/`pwritev` (KernelDevice, dispatch_io)
- `ceph_fdatasync`→`F_FULLFSYNC`; huge-page mmap `MAP_ANON` arm; `is_expected_ioerr`
  Apple arm; `cpu_set_t`/affinity shim; POSIX-timer + `memset_s`/`memzero` guards
- `sigdescr_np`→`strsignal`; `Journald.cc` (sockaddr_un sun_len, memfd, SOCK_CLOEXEC,
  getprogname, thread-id fmt); `tcp_info` no-op; `PROCPREFIX`; blkdev metrics/smartctl
  stubs (the last ceph-mon link symbol)

## Run against 4 hdiutil-backed BlueStore OSDs

4 `hdiutil` HFS+ sparse volumes (`/Volumes/cephosd0..3`) each host a BlueStore
block file. `vstart.sh --bluestore --bluestore-devs <files>` stands up
1 mon + 4 OSDs (BlueStore → `darwin_gcd_queue_t`, `bdev_dispatchio=true`) + 1 mds;
`ceph fs new` + `ceph-fuse` (fuse-t via `NFSSRV_PATH`, no sudo) mounts CephFS.
(Bring-up runbook: `bringup.sh` / `cephenv.sh`.)
