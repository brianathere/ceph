# darwin_gcd_queue_t standalone validator

Functional test for the macOS libdispatch+kqueue BlueStore async I/O backend
(`src/blk/kernel/dispatch_io.cc`, `darwin_gcd_queue_t`).

A full Ceph build on macOS needs boost/rocksdb and the rest of the tree, so this
validator compiles **the real `dispatch_io.cc`** against thin stub headers
(`stubs/`) that provide just the `io_queue_t`/`aio_t` surface the backend uses,
links it against the system `libdispatch`/`kqueue`, and exercises it on a real
file. It validates the parts of the backend that are independent of the rest of
BlueStore: the `io_queue_t` contract, completion delivery, and the kqueue
doorbell.

## Run

```sh
sh run.sh
```

Requires Apple clang (for `-fblocks`) — the default macOS toolchain.

## What it checks

- **256 concurrent single-iovec writes** — exactly-once completion, `rval == length`.
- **16 multi-iovec (3-segment) writes** — exercises the `pwritev` iovcnt>1 path and
  the short-transfer iovec-advance loop.
- **256 reads + byte-for-byte verification** — correct offsets and zero-copy into `iov`.
- **kqueue `EVFILT_USER` doorbell** — a submitter thread fires one I/O after 150 ms
  while the reaper blocks with a 5000 ms timeout; the test asserts the reaper wakes
  in < 1500 ms, proving the doorbell (not the timeout) drives completions.
- **Clean `shutdown()`**.

## Expected output

```
  [A: 256 writes] reaped 256/256 in 0 ms, all exactly-once, rval==length
  [B: 16 multi-iov writes] reaped 16/16 in 0 ms, all exactly-once, rval==length
  [C: 256 reads] reaped 256/256 in 0 ms, all exactly-once, rval==length
  [C verify] 256 regions, 0 mismatched
  [D doorbell] reaper woke ~150 ms after submit (timeout was 5000 ms)
  [shutdown] clean

ALL TESTS PASSED
```

## Scope / caveats

This is not a substitute for a full BlueStore objectstore test (which requires a
complete macOS Ceph build). It validates the backend's concurrency/completion
machinery in isolation. End-to-end CephFS-over-macFUSE additionally needs the
mon/mgr/osd/mds daemons built and macFUSE installed.
