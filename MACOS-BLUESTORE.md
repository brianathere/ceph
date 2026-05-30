# Native macOS BlueStore + idiomatic libdispatch/kqueue async backend

This branch (`macos-bluestore-gcd`) brings Ceph BlueStore to macOS and adds an
idiomatic Darwin-native async I/O backend, `darwin_gcd_queue_t`.

## What's here

- **Enable BlueStore on macOS** on top of the FreeBSD-style POSIX infrastructure
  (`HAVE_POSIXAIO` keeps the shared `aio_t`/`IOContext` plumbing compiled).
- **`darwin_gcd_queue_t`** (`src/blk/kernel/dispatch_io.{h,cc}`) — a new
  `io_queue_t` backend that runs blocking `pwritev`/`preadv` on a **private Grand
  Central Dispatch concurrent queue** (zero-copy into `aio_t.iov`), bounds
  in-flight I/O with a `dispatch_semaphore` to prevent thread explosion, and
  delivers completions to the reaper via a **kqueue `EVFILT_USER` doorbell**.
  This sidesteps POSIX AIO's `kern.aioprocmax=16` per-process ceiling. Durability
  stays in `KernelDevice::flush()` → `fcntl(F_FULLFSYNC)`.
- Durability/compat helpers (`ceph_fdatasync`→`F_FULLFSYNC`,
  `ceph_set_nocache`→`F_NOCACHE`, dropped fake `O_DIRECT`).
- All changes are `#if`-guarded; Linux/FreeBSD builds are unaffected.

Config: `bdev_dispatchio` (default true on macOS), `bdev_dispatchio_max_threads`
(default 64).

## Status

| Item | State |
|---|---|
| Backend compiles on macOS (Apple clang 21, arm64) | ✅ verified (standalone) |
| Functional tests | ✅ pass (see below) |
| Code review (2 adversarial passes) | ✅ done; fixes applied |
| Full Ceph build on macOS | ⚠️ not done — needs boost/rocksdb + the broader macOS port (mon/osd/mds/msg); see "Full build" |
| CephFS over macFUSE end-to-end | ⚠️ blocked — needs the full build + macFUSE (system extension + reboot) + a backing disk |

## Run the backend validator (no full build needed)

```sh
sh src/test/blk/darwin_gcd/run.sh
```

Compiles the **real** `src/blk/kernel/dispatch_io.cc` against thin stubs +
libdispatch/kqueue and exercises it on a temp file. Validates: 256 concurrent
writes, multi-iovec writes, 256 reads with byte-for-byte verification, the
kqueue `EVFILT_USER` doorbell wakeup latency, and clean shutdown. Passes on
macOS 26.5 / arm64 / Apple clang 21.

## Full build (the larger macOS port — not yet complete)

BlueStore's block layer is ported here, but a full `ceph-osd`/`ceph-fuse` build
also requires the rest of the tree to compile on macOS. Prerequisites:

```sh
brew install llvm cmake ninja boost snappy pkg-config
git submodule update --init --recursive        # rocksdb, etc.
mkdir build && cd build
cmake .. -DWITH_BLUESTORE=ON -DWITH_CEPHFS=ON -DWITH_LIBCEPHFS=ON \
         -DWITH_FUSE=ON -DWITH_LIBURING=OFF -DWITH_SPDK=OFF \
         -DWITH_XFS=OFF -DWITH_RDMA=OFF -DALLOCATOR=libc
ninja blk            # the part this branch targets
# ninja ceph-osd ceph-mon ceph-mgr ceph-mds ceph-fuse   # needs the broader port
```

The broader port (mon/osd/msg/mds Linux-isms) is tracked separately and is a
larger effort; this branch is scoped to the BlueStore block layer + backend.

## CephFS over macFUSE (end-to-end) — runbook

Once the full build above succeeds:

1. Install macFUSE (one-time, **requires admin + a reboot** to approve the
   system extension): `brew install --cask macfuse`, then approve it in
   System Settings → Privacy & Security and reboot.
2. Provide a backing block device for the OSD — a raw `/dev/rdiskN` (e.g. a
   DriverKit/DEXT LUN, or `hdiutil attach -nomount` of a sparse image).
3. Stand up a single-node cluster (`mon`+`mgr`+`osd` on the device+`mds`),
   `ceph fs new`, then mount: `ceph-fuse /mnt/cephfs`.
4. Durability check: write → `sync` → hard-kill the OSD → remount → scrub;
   confirm no acknowledged writes are lost (validates the `F_FULLFSYNC`
   barrier through to the device).
