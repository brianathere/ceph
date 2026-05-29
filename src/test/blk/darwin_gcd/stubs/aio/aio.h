#pragma once
// Test-only minimal stand-in for src/blk/aio/aio.h, exposing exactly the
// aio_t fields and the io_queue_t interface that darwin_gcd_queue_t
// (dispatch_io.{h,cc}) compiles against. Signatures match the real header so
// the `final` overrides bind identically.
#include <cstdint>
#include <list>
#include <vector>
#include <sys/uio.h>  // struct iovec
#include <boost/container/small_vector.hpp>

#if defined(HAVE_DARWIN_AIO)
enum darwin_aio_rw_t { DARWIN_AIO_READ = 0, DARWIN_AIO_WRITE = 1 };
#endif

struct aio_t {
  void *priv = nullptr;
  int fd = -1;
  boost::container::small_vector<iovec, 4> iov;
  uint64_t offset = 0, length = 0;
  long rval = -1000;
#if defined(HAVE_DARWIN_AIO)
  int rw = DARWIN_AIO_READ;
#endif

  aio_t(void *p, int f) : priv(p), fd(f) {}

  void pwritev(uint64_t _offset, uint64_t len) {
    offset = _offset;
    length = len;
#if defined(HAVE_DARWIN_AIO)
    rw = DARWIN_AIO_WRITE;
#endif
  }
  void preadv(uint64_t _offset, uint64_t len) {
    offset = _offset;
    length = len;
#if defined(HAVE_DARWIN_AIO)
    rw = DARWIN_AIO_READ;
#endif
  }
  long get_return_value() { return rval; }
};

struct io_queue_t {
  typedef std::list<aio_t>::iterator aio_iter;
  virtual ~io_queue_t() {}
  virtual int init(std::vector<int> &fds) = 0;
  virtual void shutdown() = 0;
  virtual int submit_batch(aio_iter begin, aio_iter end, void *priv,
                           int *retries, int submit_retries,
                           int initial_delay_us) = 0;
  virtual int get_next_completed(int timeout_ms, aio_t **paio, int max) = 0;
};
