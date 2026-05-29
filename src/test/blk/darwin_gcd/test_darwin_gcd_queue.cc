// Standalone functional test for the REAL src/blk/kernel/dispatch_io.cc
// (darwin_gcd_queue_t), compiled against thin stub headers + libdispatch/kqueue
// on a live macOS host. Validates the io_queue_t contract the backend must obey:
//   - exactly-one completion per submitted aio
//   - rval == length on success (exact byte count, incl. multi-iovec)
//   - correct data (read-back matches written pattern, offsets honored)
//   - the kqueue EVFILT_USER doorbell actually wakes the reaper (latency probe)
//   - clean shutdown
//
// Build: see build.sh
#include "dispatch_io.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

using clk = std::chrono::steady_clock;
static long ms_since(clk::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(clk::now() - t0).count();
}

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
  std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); g_fail++; } } while (0)

// Deterministic per-file-offset byte so reads can be verified independently.
static inline uint8_t expect_byte(uint64_t p) {
  uint64_t x = p * 0x9E3779B97F4A7C15ull;
  return (uint8_t)((x >> 33) ^ p);
}

static int dummy_ioc = 0;  // stands in for the IOContext* priv

// Reap exactly `n` completions, asserting exactly-once + rval==length.
static void reap_and_verify(io_queue_t& q, int n, const char* phase) {
  std::unordered_map<aio_t*, int> seen;
  int reaped = 0;
  auto t0 = clk::now();
  while (reaped < n) {
    aio_t* batch[16];
    int r = q.get_next_completed(2000, batch, 16);
    CHECK(r >= 0, "get_next_completed returned error");
    if (r < 0) return;
    for (int i = 0; i < r; ++i) {
      aio_t* a = batch[i];
      CHECK(a->rval == (long)a->length, "rval != length (short/failed transfer)");
      int c = ++seen[a];
      CHECK(c == 1, "aio completed more than once");
      ++reaped;
    }
    CHECK(ms_since(t0) < 15000, "timed out waiting for completions");
    if (ms_since(t0) >= 15000) return;
  }
  std::printf("  [%s] reaped %d/%d in %ld ms, all exactly-once, rval==length\n",
              phase, reaped, n, ms_since(t0));
}

int main() {
  const char* path = "/tmp/darwin_gcd_test.img";
  int fd = ::open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) { std::perror("open"); return 2; }
  if (::ftruncate(fd, 64ull << 20) != 0) { std::perror("ftruncate"); return 2; }

  darwin_gcd_queue_t q(/*iodepth*/1024, /*threads*/8);
  std::vector<int> fds{fd};
  CHECK(q.init(fds) == 0, "init failed");

  std::vector<void*> bufs;  // keep all I/O buffers alive across submit+reap
  auto mkbuf = [&](uint64_t len) -> uint8_t* {
    void* b = nullptr;
    if (posix_memalign(&b, 4096, len) != 0) { std::abort(); }
    bufs.push_back(b);
    return (uint8_t*)b;
  };

  // ---- Phase A: 256 concurrent single-iovec writes -----------------------
  const int NA = 256;
  const uint64_t LEN = 64 * 1024;
  std::list<aio_t> wa;
  for (int i = 0; i < NA; ++i) {
    uint64_t off = (uint64_t)i * LEN;
    uint8_t* b = mkbuf(LEN);
    for (uint64_t k = 0; k < LEN; ++k) b[k] = expect_byte(off + k);
    wa.emplace_back(&dummy_ioc, fd);
    aio_t& a = wa.back();
    a.iov.push_back(iovec{b, LEN});
    a.pwritev(off, LEN);
  }
  int retries = 0;
  int sub = q.submit_batch(wa.begin(), wa.end(), &dummy_ioc, &retries, 16, 125);
  CHECK(sub == NA, "submit_batch did not accept all writes");
  reap_and_verify(q, NA, "A: 256 writes");

  // ---- Phase B: multi-iovec writes (exercise iov-advance + pwritev iovcnt>1)
  const int NB = 16;
  const uint64_t BASE_B = 32ull << 20;  // 32 MiB region, clear of phase A
  const uint64_t SEG = 24 * 1024;       // 3 segments => 72 KiB each
  std::list<aio_t> wb;
  for (int i = 0; i < NB; ++i) {
    uint64_t off = BASE_B + (uint64_t)i * (3 * SEG);
    wb.emplace_back(&dummy_ioc, fd);
    aio_t& a = wb.back();
    for (int s = 0; s < 3; ++s) {
      uint8_t* b = mkbuf(SEG);
      uint64_t seg_off = off + (uint64_t)s * SEG;
      for (uint64_t k = 0; k < SEG; ++k) b[k] = expect_byte(seg_off + k);
      a.iov.push_back(iovec{b, SEG});
    }
    a.pwritev(off, 3 * SEG);
  }
  retries = 0;
  sub = q.submit_batch(wb.begin(), wb.end(), &dummy_ioc, &retries, 16, 125);
  CHECK(sub == NB, "submit_batch did not accept all multi-iov writes");
  reap_and_verify(q, NB, "B: 16 multi-iov writes");

  // ---- Phase C: read everything back and verify the bytes ----------------
  std::list<aio_t> ra;
  std::vector<std::pair<uint8_t*, uint64_t>> rverify;  // (buf, file_off)
  for (int i = 0; i < NA; ++i) {
    uint64_t off = (uint64_t)i * LEN;
    uint8_t* b = mkbuf(LEN);
    std::memset(b, 0, LEN);
    ra.emplace_back(&dummy_ioc, fd);
    aio_t& a = ra.back();
    a.iov.push_back(iovec{b, LEN});
    a.preadv(off, LEN);
    rverify.push_back({b, off});
  }
  retries = 0;
  sub = q.submit_batch(ra.begin(), ra.end(), &dummy_ioc, &retries, 16, 125);
  CHECK(sub == NA, "submit_batch did not accept all reads");
  reap_and_verify(q, NA, "C: 256 reads");

  int bad = 0;
  for (auto& [buf, off] : rverify)
    for (uint64_t k = 0; k < LEN; ++k)
      if (buf[k] != expect_byte(off + k)) { ++bad; break; }
  CHECK(bad == 0, "read-back data mismatch");
  std::printf("  [C verify] %zu regions, %d mismatched\n", rverify.size(), bad);

  // ---- Phase D: kqueue EVFILT_USER doorbell wakeup latency ----------------
  // Reaper blocks with a 5s timeout; a submitter thread fires one write after
  // ~150ms. If the doorbell works the reaper wakes in ~150ms, NOT ~5000ms.
  std::list<aio_t> wd;
  {
    uint8_t* b = mkbuf(LEN);
    for (uint64_t k = 0; k < LEN; ++k) b[k] = expect_byte(k);
    wd.emplace_back(&dummy_ioc, fd);
    wd.back().iov.push_back(iovec{b, LEN});
    wd.back().pwritev(0, LEN);
  }
  std::atomic<bool> submitted{false};
  auto t0 = clk::now();
  std::thread submitter([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    int rr = 0;
    q.submit_batch(wd.begin(), wd.end(), &dummy_ioc, &rr, 16, 125);
    submitted.store(true);
  });
  aio_t* batch[4];
  int r = 0;
  while (r == 0) r = q.get_next_completed(5000, batch, 4);
  long lat = ms_since(t0);
  submitter.join();
  CHECK(r == 1, "doorbell test: expected exactly one completion");
  CHECK(lat < 1500, "doorbell did NOT wake reaper (woke via timeout, not EVFILT_USER)");
  std::printf("  [D doorbell] reaper woke %ld ms after submit (timeout was 5000 ms)\n", lat);

  // ---- shutdown -----------------------------------------------------------
  q.shutdown();
  std::printf("  [shutdown] clean\n");

  ::close(fd);
  ::unlink(path);
  for (void* b : bufs) free(b);

  std::printf(g_fail == 0 ? "\nALL TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
