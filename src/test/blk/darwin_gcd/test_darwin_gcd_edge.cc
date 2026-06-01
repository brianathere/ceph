// Edge-case / error-path validator for the REAL src/blk/kernel/dispatch_io.cc
// (darwin_gcd_queue_t). The companion test_darwin_gcd_queue.cc covers the
// happy path (full transfers on a regular file, where preadv/pwritev never
// short-transfer); by construction it cannot reach the backend's error,
// EOF-short, partial-iovec-trim, timeout, empty-batch or drain-tail branches.
// This test drives exactly those, using deterministic EOF / EBADF conditions
// so no flaky signal/timing tricks are needed.
//
// Branches asserted here (file refs are dispatch_io.cc):
//   - submit_batch with begin==end -> returns 0, no work dispatched
//   - get_next_completed timeout (kevent r==0) -> returns 0 after ~timeout
//   - worker error path: pwritev on an O_RDONLY fd -> rval == -EBADF  (line ~172)
//   - EOF short read: preadv at offset==size -> rval == 0            (line ~175)
//   - single-iovec partial short read: read straddling EOF           (advance ~180-184)
//   - multi-iovec short read: full first iovec + partial second      (trim ~185-188)
//   - zero-length iovec inside a multi-iovec request is skipped cleanly
//   - max=1 drain-tail: N completions all delivered one-at-a-time even
//     though a single EVFILT_USER trigger can be consumed for several (line ~223-231)
//
// Build: see run.sh (compiled + run alongside test_darwin_gcd_queue.cc).
#include "dispatch_io.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <vector>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

using clk = std::chrono::steady_clock;
static long ms_since(clk::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
  std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); g_fail++; } } while (0)

static int dummy_ioc = 0;

static std::vector<void*> g_bufs;
static uint8_t* mkbuf(uint64_t len) {
  void* b = nullptr;
  if (posix_memalign(&b, 4096, len ? len : 4096) != 0) std::abort();
  g_bufs.push_back(b);
  return (uint8_t*)b;
}

// Reap exactly one aio (the tests below submit singly or drain one-at-a-time)
// and return its rval. Allows rval != length (that is the whole point here).
static long reap_one(io_queue_t& q) {
  auto t0 = clk::now();
  for (;;) {
    aio_t* batch[8];
    int r = q.get_next_completed(2000, batch, 8);
    CHECK(r >= 0, "get_next_completed error while reaping one");
    if (r > 0) {
      CHECK(r == 1, "reap_one expected a single completion in this phase");
      return batch[0]->rval;
    }
    if (ms_since(t0) >= 10000) { CHECK(false, "reap_one timed out"); return -1; }
  }
}

int main() {
  const char* path = "/tmp/darwin_gcd_edge.img";
  const uint64_t SIZE = 8 * 1024;             // small file so EOF is easy to hit
  int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { std::perror("open"); return 2; }
  if (::ftruncate(fd, SIZE) != 0) { std::perror("ftruncate"); return 2; }
  // Fill with a known marker so short reads are visibly bounded.
  {
    uint8_t* fillb = mkbuf(SIZE);
    std::memset(fillb, 0xAB, SIZE);
    if (::pwrite(fd, fillb, SIZE, 0) != (ssize_t)SIZE) { std::perror("pwrite fill"); return 2; }
  }
  int ro_fd = ::open(path, O_RDONLY);          // for the write-error path
  if (ro_fd < 0) { std::perror("open ro"); return 2; }

  darwin_gcd_queue_t q(/*iodepth*/64, /*threads*/4);
  std::vector<int> fds{fd};
  CHECK(q.init(fds) == 0, "init failed");

  // ---- E1: empty batch -> 0, nothing dispatched --------------------------
  {
    std::list<aio_t> empty;
    int rr = 0;
    int sub = q.submit_batch(empty.begin(), empty.end(), &dummy_ioc, &rr, 16, 125);
    CHECK(sub == 0, "empty submit_batch should return 0");
    std::printf("  [E1 empty-batch] submit_batch(empty)=%d\n", sub);
  }

  // ---- E2: timeout path (no pending IO) ----------------------------------
  {
    aio_t* batch[4];
    auto t0 = clk::now();
    int r = q.get_next_completed(200, batch, 4);
    long waited = ms_since(t0);
    CHECK(r == 0, "get_next_completed with no work should return 0 (timeout)");
    CHECK(waited >= 150, "timeout returned far too early (kevent timespec wrong?)");
    std::printf("  [E2 timeout] returned %d after %ld ms (asked 200)\n", r, waited);
  }

  // ---- E3: worker error path -> rval == -EBADF ---------------------------
  // pwritev() on an O_RDONLY fd fails with EBADF; backend must report -errno.
  {
    std::list<aio_t> w;
    uint8_t* b = mkbuf(4096);
    std::memset(b, 0x5A, 4096);
    w.emplace_back(&dummy_ioc, ro_fd);
    w.back().iov.push_back(iovec{b, 4096});
    w.back().pwritev(0, 4096);
    int rr = 0;
    int sub = q.submit_batch(w.begin(), w.end(), &dummy_ioc, &rr, 16, 125);
    CHECK(sub == 1, "submit of error-write not accepted");
    long rval = reap_one(q);
    CHECK(rval == -EBADF, "write to O_RDONLY fd should yield rval == -EBADF");
    std::printf("  [E3 error] pwritev(O_RDONLY) rval=%ld (expected %d)\n", rval, -EBADF);
  }

  // ---- E4: EOF short read, single iovec at offset==size -> rval == 0 ------
  {
    std::list<aio_t> r;
    uint8_t* b = mkbuf(4096);
    r.emplace_back(&dummy_ioc, fd);
    r.back().iov.push_back(iovec{b, 4096});
    r.back().preadv(SIZE, 4096);              // start exactly at EOF
    int rr = 0;
    q.submit_batch(r.begin(), r.end(), &dummy_ioc, &rr, 16, 125);
    long rval = reap_one(q);
    CHECK(rval == 0, "preadv at EOF should yield rval == 0");
    std::printf("  [E4 eof] preadv@EOF rval=%ld (expected 0)\n", rval);
  }

  // ---- E5: single-iovec partial short read straddling EOF ----------------
  // Read 4096 starting 1024 before EOF -> exactly 1024 available.
  {
    std::list<aio_t> r;
    uint8_t* b = mkbuf(4096);
    std::memset(b, 0, 4096);
    r.emplace_back(&dummy_ioc, fd);
    r.back().iov.push_back(iovec{b, 4096});
    r.back().preadv(SIZE - 1024, 4096);
    int rr = 0;
    q.submit_batch(r.begin(), r.end(), &dummy_ioc, &rr, 16, 125);
    long rval = reap_one(q);
    CHECK(rval == 1024, "straddle-EOF single-iovec read should yield exactly 1024");
    int good = 1;
    for (int k = 0; k < 1024; ++k) if (b[k] != 0xAB) good = 0;
    CHECK(good, "straddle read returned wrong bytes");
    std::printf("  [E5 short-1iov] rval=%ld (expected 1024), bytes ok=%d\n", rval, good);
  }

  // ---- E6: multi-iovec short read: full iov0 + partial iov1 (trim branch) -
  // File 8K; read at off=1024 with iov0=4096 + iov1=4096 (req 8192).
  // Available = 8192-1024 = 7168 -> iov0 full (4096) + 3072 into iov1.
  // Exercises BOTH the boundary-advance (iov0) and the partial-trim (iov1).
  {
    std::list<aio_t> r;
    uint8_t* b0 = mkbuf(4096);
    uint8_t* b1 = mkbuf(4096);
    std::memset(b0, 0, 4096); std::memset(b1, 0, 4096);
    r.emplace_back(&dummy_ioc, fd);
    aio_t& a = r.back();
    a.iov.push_back(iovec{b0, 4096});
    a.iov.push_back(iovec{b1, 4096});
    a.preadv(1024, 8192);
    int rr = 0;
    q.submit_batch(r.begin(), r.end(), &dummy_ioc, &rr, 16, 125);
    long rval = reap_one(q);
    CHECK(rval == 7168, "multi-iovec straddle read should yield exactly 7168");
    int good = 1;
    for (int k = 0; k < 4096; ++k) if (b0[k] != 0xAB) good = 0;   // iov0 full
    for (int k = 0; k < 3072; ++k) if (b1[k] != 0xAB) good = 0;   // iov1 partial
    for (int k = 3072; k < 4096; ++k) if (b1[k] != 0) good = 0;   // untouched tail
    CHECK(good, "multi-iovec straddle returned wrong bytes / overwrote tail");
    std::printf("  [E6 short-2iov] rval=%ld (expected 7168), bytes ok=%d\n", rval, good);
  }

  // ---- E7: zero-length iovec in the middle is skipped cleanly ------------
  // iov0=2048, iov1=0 (zero-length), iov2=2048 ; full read inside the file.
  {
    std::list<aio_t> r;
    uint8_t* b0 = mkbuf(2048);
    uint8_t* b2 = mkbuf(2048);
    std::memset(b0, 0, 2048); std::memset(b2, 0, 2048);
    r.emplace_back(&dummy_ioc, fd);
    aio_t& a = r.back();
    a.iov.push_back(iovec{b0, 2048});
    a.iov.push_back(iovec{(void*)b0, 0});   // zero-length
    a.iov.push_back(iovec{b2, 2048});
    a.preadv(0, 4096);
    int rr = 0;
    q.submit_batch(r.begin(), r.end(), &dummy_ioc, &rr, 16, 125);
    long rval = reap_one(q);
    CHECK(rval == 4096, "read with a zero-length iovec should still total 4096");
    int good = 1;
    for (int k = 0; k < 2048; ++k) if (b0[k] != 0xAB || b2[k] != 0xAB) good = 0;
    CHECK(good, "zero-length-iovec read returned wrong bytes");
    std::printf("  [E7 zero-iov] rval=%ld (expected 4096), bytes ok=%d\n", rval, good);
  }

  // ---- E8: max=1 drain-tail ----------------------------------------------
  // Submit 8 writes, then reap with max=1 repeatedly. All 8 must be delivered
  // even though one EVFILT_USER delivery can correspond to several completions
  // (the drain-first path must hand back the stranded tail without blocking).
  {
    const int N = 8;
    std::list<aio_t> w;
    for (int i = 0; i < N; ++i) {
      uint8_t* b = mkbuf(512);
      std::memset(b, 0x11 + i, 512);
      w.emplace_back(&dummy_ioc, fd);
      w.back().iov.push_back(iovec{b, 512});
      w.back().pwritev((uint64_t)i * 512, 512);
    }
    int rr = 0;
    int sub = q.submit_batch(w.begin(), w.end(), &dummy_ioc, &rr, 16, 125);
    CHECK(sub == N, "drain-tail: not all writes accepted");
    int reaped = 0;
    auto t0 = clk::now();
    while (reaped < N) {
      aio_t* one[1];
      int r = q.get_next_completed(2000, one, 1);   // max == 1
      CHECK(r >= 0, "drain-tail get_next_completed error");
      CHECK(r <= 1, "drain-tail honored max=1");
      reaped += (r > 0 ? 1 : 0);
      if (ms_since(t0) >= 10000) { CHECK(false, "drain-tail timed out"); break; }
    }
    CHECK(reaped == N, "drain-tail did not deliver all completions one-at-a-time");
    std::printf("  [E8 drain-tail] delivered %d/%d with max=1\n", reaped, N);
  }

  // ---- E9: negative poll timeout is clamped, not a fatal EINVAL ----------
  // A misconfigured (negative) bdev_aio_poll_ms would build an invalid timespec
  // {neg, neg}; kevent() then fails EINVAL and the reaper escalates that to a
  // ceph_abort. get_next_completed must clamp a negative timeout to a 0ms
  // non-blocking poll and return 0 (no work pending), never -EINVAL.
  {
    aio_t* batch[4];
    int r = q.get_next_completed(-250, batch, 4);
    CHECK(r == 0, "negative timeout must clamp to a 0ms poll and return 0, not -EINVAL");
    std::printf("  [E9 neg-timeout] get_next_completed(-250)=%d (expected 0)\n", r);
  }

  q.shutdown();
  std::printf("  [shutdown] clean\n");

  ::close(ro_fd);
  ::close(fd);
  ::unlink(path);
  for (void* b : g_bufs) free(b);

  std::printf(g_fail == 0 ? "\nEDGE TESTS PASSED\n" : "\n%d EDGE CHECK(S) FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
