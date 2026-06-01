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

`run.sh` builds and runs three binaries:

### `run` — happy-path `io_queue_t` contract (`test_darwin_gcd_queue.cc`)
- **256 concurrent single-iovec writes** — exactly-once completion, `rval == length`.
- **16 multi-iovec (3-segment) writes** — exercises the `pwritev` iovcnt>1 path and
  the short-transfer iovec-advance loop.
- **256 reads + byte-for-byte verification** — correct offsets and zero-copy into `iov`.
- **kqueue `EVFILT_USER` doorbell** — a submitter thread fires one I/O after 150 ms
  while the reaper blocks with a 5000 ms timeout; the test asserts the reaper wakes
  in < 1500 ms, proving the doorbell (not the timeout) drives completions.
- **Clean `shutdown()`**.

### `run_edge` — error / edge-path branches (`test_darwin_gcd_edge.cc`)
The happy-path test only does full transfers on a regular file, where
`preadv`/`pwritev` never short-transfer, so it cannot reach the backend's error,
EOF, partial-iovec-trim, timeout, empty-batch or drain-tail branches. This test
drives each deterministically (EOF / EBADF, no timing tricks):
- **empty batch** → `submit_batch` returns 0;
- **timeout** → `get_next_completed` returns 0 after ~the requested ms;
- **error path** → `pwritev` on an `O_RDONLY` fd → `rval == -EBADF`;
- **EOF short read** → `preadv` at `offset == size` → `rval == 0`;
- **single-iovec straddle-EOF** → partial transfer, exact byte count + bytes;
- **multi-iovec straddle-EOF** → full iov0 + partial iov1 (the iovec **trim**
  arithmetic), exact count, untouched tail preserved;
- **zero-length iovec** mid-list is skipped cleanly;
- **`max == 1` drain-tail** → 8 completions all delivered one-at-a-time even though
  a single `EVFILT_USER` delivery can cover several (the drain-first path).

### `run_clsname` — cls name-derivation guard (`test_cls_name_filter.cc`)
Replicates `ClassHandler::open_all_classes()`'s filename → class-name derivation and
the macOS versioned-dylib skip, asserting classification for `.dylib` (macOS) and
`.so` (Linux) entries: canonical `libcls_journal.dylib` → `journal`; versioned
`libcls_journal.1.dylib` / `.1.0.0.dylib` skipped; real names with digits/underscores
(`2pc_queue`, `rgw_gc`) preserved; non-cls and wrong-suffix entries rejected. Guards
the fix for the OSD-startup `SIGSEGV` where a versioned dylib was loaded as class
`journal.1`, `register_class` returned `NULL`, and the plugin dereferenced it.

## Expected output

```
== happy-path contract test ==
  [A: 256 writes] reaped 256/256 ..., all exactly-once, rval==length
  [B: 16 multi-iov writes] reaped 16/16 ..., all exactly-once, rval==length
  [C: 256 reads] reaped 256/256 ..., all exactly-once, rval==length
  [C verify] 256 regions, 0 mismatched
  [D doorbell] reaper woke ~150 ms after submit (timeout was 5000 ms)
  [shutdown] clean
ALL TESTS PASSED

== edge / error-path test ==
  [E1 empty-batch] ... [E2 timeout] ... [E3 error] ... [E4 eof] ...
  [E5 short-1iov] ... [E6 short-2iov] ... [E7 zero-iov] ... [E8 drain-tail] ...
EDGE TESTS PASSED

== cls name-derivation guard ==
  ok: libcls_journal.dylib -> journal ;  libcls_journal.1.dylib -> <skip-versioned> ; ...
CLS NAME-FILTER TESTS PASSED
```

## Scope / caveats

This is not a substitute for a full BlueStore objectstore test (which requires a
complete macOS Ceph build). It validates the backend's concurrency/completion
machinery in isolation. End-to-end CephFS-over-macFUSE additionally needs the
mon/mgr/osd/mds daemons built and macFUSE installed.
