// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include "acconfig.h"

#include "include/types.h"
#include "aio/aio.h"

#include <list>
#include <memory>
#include <vector>

// Opaque state so <dispatch/dispatch.h> stays out of this header (it is pulled
// in by BlockDevice.h / KernelDevice.cc and we do not want libdispatch types
// leaking platform-wide).
struct darwin_gcd_data;

/*
 * darwin_gcd_queue_t - an idiomatic macOS async block-I/O backend.
 *
 * macOS POSIX AIO (aio_read/aio_write/lio_listio) is unusable for a storage
 * backend: kern.aioprocmax caps a process at ~16 in-flight requests. Instead
 * this backend runs blocking pwritev()/preadv() on a *private* libdispatch
 * concurrent queue (so GCD owns thread scheduling + QoS), bounds the number of
 * concurrently blocked worker threads with a dispatch_semaphore (avoiding GCD
 * thread explosion), and bridges completions back to KernelDevice's single
 * reaper thread through a kqueue EVFILT_USER "doorbell" + a lock-protected
 * completion deque. This satisfies the io_queue_t contract exactly like
 * aio_queue_t/ioring_queue_t, with no POSIX-aio concurrency ceiling and with
 * zero-copy I/O straight into aio_t.iov.
 *
 * Durability is NOT this backend's job: completion only means "written/queued".
 * Stable-media durability remains the separate KernelDevice::flush() barrier
 * (ceph_fdatasync -> fcntl(F_FULLFSYNC)), identical to the libaio path.
 */
struct darwin_gcd_queue_t final : public io_queue_t {
  std::unique_ptr<darwin_gcd_data> d;
  unsigned max_iodepth = 0;
  unsigned max_threads = 0;

  typedef std::list<aio_t>::iterator aio_iter;

  // libdispatch ships in libSystem on every macOS, so this is always true on
  // Apple; provided for symmetry with ioring_queue_t::supported().
  static bool supported();

  darwin_gcd_queue_t(unsigned iodepth, unsigned threads);
  ~darwin_gcd_queue_t() final;

  int init(std::vector<int> &fds) final;
  void shutdown() final;

  int submit_batch(aio_iter begin, aio_iter end,
                   void *priv, int *retries, int submit_retries,
                   int initial_delay_us) final;
  int get_next_completed(int timeout_ms, aio_t **paio, int max) final;
};
