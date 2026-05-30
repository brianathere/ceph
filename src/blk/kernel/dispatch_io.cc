// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

// macOS declares preadv()/pwritev() in <sys/uio.h> only when the Darwin
// extensions are visible; Ceph's strict compile mode (_POSIX_C_SOURCE) hides
// them. This MUST precede ALL includes — dispatch_io.h transitively pulls
// <sys/uio.h> — so it is the very first thing in the file.
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "dispatch_io.h"

#if defined(HAVE_DARWIN_AIO)

#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>
#include <boost/container/small_vector.hpp>  // explicit: used in the worker block

#include <dispatch/dispatch.h>
#include <sys/event.h>
#include <sys/uio.h>        // preadv / pwritev / struct iovec
#include <unistd.h>
#include <errno.h>

#include "include/ceph_assert.h"

namespace {
// kqueue ident for the user-triggered completion doorbell. Any constant works;
// we only ever register one EVFILT_USER on this queue.
constexpr uintptr_t DOORBELL_IDENT = 1;
}

struct darwin_gcd_data {
  int kq = -1;                          // kqueue carrying the EVFILT_USER doorbell
  dispatch_queue_t io_q = nullptr;      // PRIVATE concurrent queue running blocking pio
  dispatch_semaphore_t depth = nullptr; // bounds in-flight IOs => bounds blocked threads
  std::mutex lock;                      // guards `completed`
  std::deque<aio_t*> completed;         // finished aios awaiting the reaper

  // Best-effort cleanup if shutdown() was never called (abnormal teardown).
  // The normal path runs shutdown() first, which nulls these out, so this is
  // then a no-op; it keeps us from leaking the kqueue fd / dispatch objects
  // the way aio_queue_t avoids leaking its io_context.
  ~darwin_gcd_data() {
    if (io_q) dispatch_release(io_q);
    if (depth) dispatch_release(depth);
    if (kq >= 0) ::close(kq);
  }
};

bool darwin_gcd_queue_t::supported()
{
  return true;  // libdispatch is part of libSystem on macOS
}

darwin_gcd_queue_t::darwin_gcd_queue_t(unsigned iodepth, unsigned threads)
  : d(std::make_unique<darwin_gcd_data>()),
    max_iodepth(iodepth),
    max_threads(threads ? threads : 64)
{
}

darwin_gcd_queue_t::~darwin_gcd_queue_t() = default;

int darwin_gcd_queue_t::init(std::vector<int> &fds)
{
  (void)fds;  // I/O is issued on aio_t.fd directly; no per-fd channels needed.

  d->kq = kqueue();
  if (d->kq < 0)
    return -errno;

  // Register the completion doorbell. EV_CLEAR makes the trigger auto-reset on
  // each delivery; NOTE_TRIGGER (fired from the worker blocks) latches in the
  // kernel, so a trigger that races ahead of get_next_completed()'s kevent()
  // is not lost.
  struct kevent kev;
  EV_SET(&kev, DOORBELL_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
  if (kevent(d->kq, &kev, 1, nullptr, 0, nullptr) < 0) {
    int e = errno;
    ::close(d->kq);
    d->kq = -1;
    return -e;
  }

  // A PRIVATE concurrent queue (never a global one): worker blocks make blocking
  // syscalls, and we must not stall a shared queue. QOS_CLASS_UTILITY matches
  // background storage I/O.
  dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(
    DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_UTILITY, 0);
  d->io_q = dispatch_queue_create("ceph-bstore-gcd", attr);

  // Bound concurrently-blocked worker threads. Each in-flight blocking pio holds
  // one GCD thread, so we cap at max_threads to prevent thread explosion;
  // submit_batch() blocks for backpressure once the bound is reached.
  unsigned bound = max_threads;
  if (max_iodepth && max_iodepth < bound)
    bound = max_iodepth;
  if (bound == 0)
    bound = 1;
  d->depth = dispatch_semaphore_create(bound);

  return 0;
}

void darwin_gcd_queue_t::shutdown()
{
  // KernelDevice::_aio_stop() has already woken + joined the reaper before
  // calling us, so no get_next_completed() is in flight here.
  if (d->io_q) {
    // Barrier on the concurrent queue: returns only after every previously
    // dispatched worker block has fully completed (and thus signalled the
    // semaphore), so it is safe to release both below.
    dispatch_barrier_sync(d->io_q, ^{});
    dispatch_release(d->io_q);
    d->io_q = nullptr;
  }
  if (d->depth) {
    dispatch_release(d->depth);
    d->depth = nullptr;
  }
  if (d->kq >= 0) {
    ::close(d->kq);
    d->kq = -1;
  }
}

int darwin_gcd_queue_t::submit_batch(aio_iter begin, aio_iter end,
                                     void *priv, int *retries,
                                     int submit_retries, int initial_delay_us)
{
  // GCD self-throttles via the depth semaphore, so the libaio-style EAGAIN
  // backoff parameters are inert here (mirrors ioring_queue_t).
  (void)retries;
  (void)submit_retries;
  (void)initial_delay_us;

  darwin_gcd_data *dd = d.get();
  int submitted = 0;

  for (aio_iter p = begin; p != end; ++p) {
    aio_t *aio = &*p;
    aio->priv = priv;

    // Backpressure: never exceed `bound` blocking workers in flight. Released by
    // the worker block on completion (independent of the reaper), so this never
    // deadlocks against the reaper or BlueStore locks.
    dispatch_semaphore_wait(dd->depth, DISPATCH_TIME_FOREVER);

    dispatch_async(dd->io_q, ^{
      const bool is_write = (aio->rw == DARWIN_AIO_WRITE);

      // Local, advanceable copy of the iovec (handles short transfers without
      // mutating the caller's aio->iov, which the reaper still inspects).
      boost::container::small_vector<iovec, 4> iov(aio->iov.begin(),
                                                   aio->iov.end());
      uint64_t off = aio->offset;
      uint64_t done = 0;
      long err = 0;
      size_t idx = 0;

      while (idx < iov.size()) {
        ssize_t n = is_write
          ? ::pwritev(aio->fd, &iov[idx], (int)(iov.size() - idx), off)
          : ::preadv(aio->fd, &iov[idx], (int)(iov.size() - idx), off);
        if (n < 0) {
          if (errno == EINTR)
            continue;
          err = -errno;
          break;
        }
        if (n == 0)
          break;  // unexpected short read at EOF; rval!=length aborts upstream
        done += (uint64_t)n;
        off += (uint64_t)n;
        // Advance past fully-consumed iovecs, then trim the partial one.
        uint64_t adv = (uint64_t)n;
        while (idx < iov.size() && adv >= iov[idx].iov_len) {
          adv -= iov[idx].iov_len;
          ++idx;
        }
        if (idx < iov.size() && adv > 0) {
          iov[idx].iov_base = (char*)iov[idx].iov_base + adv;
          iov[idx].iov_len -= adv;
        }
      }

      // Contract: rval is the byte count (== aio->length on success) or -errno.
      aio->rval = err ? err : (long)done;

      {
        std::lock_guard<std::mutex> l(dd->lock);
        dd->completed.push_back(aio);
      }

      // Ring the doorbell to wake the reaper. NOTE_TRIGGER latches, so this is
      // safe even if the reaper is between its drain and its kevent().
      struct kevent kev;
      EV_SET(&kev, DOORBELL_IDENT, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
      kevent(dd->kq, &kev, 1, nullptr, 0, nullptr);

      dispatch_semaphore_signal(dd->depth);
    });

    ++submitted;
  }

  // Number of IOs accepted (>0). aio_submit only requires a non-negative
  // return (it asserts r >= 0), matching aio_queue_t/ioring_queue_t.
  return submitted;
}

int darwin_gcd_queue_t::get_next_completed(int timeout_ms, aio_t **paio, int max)
{
  darwin_gcd_data *dd = d.get();

  // 1) Drain-first: return already-completed aios without blocking. This also
  //    drains the tail when a previous call returned `max` and left more behind
  //    (the EVFILT_USER trigger for those may already have been consumed).
  {
    std::lock_guard<std::mutex> l(dd->lock);
    int n = 0;
    while (n < max && !dd->completed.empty()) {
      paio[n++] = dd->completed.front();
      dd->completed.pop_front();
    }
    if (n > 0)
      return n;
  }

  // 2) Block on the doorbell up to timeout_ms.
  struct timespec t = {
    timeout_ms / 1000,
    (timeout_ms % 1000) * 1000 * 1000
  };
  struct kevent ev;
  int r;
  do {
    r = kevent(dd->kq, nullptr, 0, &ev, 1, &t);
  } while (r < 0 && errno == EINTR);
  if (r < 0)
    return -errno;
  if (r == 0)
    return 0;  // timeout, nothing completed

  // 3) Drain whatever the wakeup made available (a single EVFILT_USER delivery
  //    can represent many coalesced completions).
  std::lock_guard<std::mutex> l(dd->lock);
  int n = 0;
  while (n < max && !dd->completed.empty()) {
    paio[n++] = dd->completed.front();
    dd->completed.pop_front();
  }
  return n;
}

#endif // HAVE_DARWIN_AIO
