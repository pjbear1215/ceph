
// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
  *
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <libgen.h>
#include <unistd.h>

#include "ThBlockDevice.h"

#if defined(HAVE_LIBAIO) || defined(HAVE_POSIXAIO)
#include "ThKernelDevice.h"
#endif

#if defined(HAVE_SPDK)
// ToDo support SPDK
//#include "NVMEDevice.h"
#endif

#include "common/debug.h"
#include "common/EventTrace.h"
#include "common/errno.h"
#include "include/compat.h"

#define dout_context cct
#define dout_subsys ceph_subsys_bdev
#undef dout_prefix
#define dout_prefix *_dout << "bdev "

void ThIOContext::aio_wait()
{
  std::unique_lock l(lock);
  // see _aio_thread for waker logic
  while (num_running.load() > 0) {
    dout(10) << __func__ << " " << this
	     << " waiting for " << num_running.load() << " aios to complete"
	     << dendl;
    cond.wait(l);
  }
  dout(20) << __func__ << " " << this << " done" << dendl;
}

uint64_t ThIOContext::get_num_ios() const
{
  // this is about the simplest model for transaction cost you can
  // imagine.  there is some fixed overhead cost by saying there is a
  // minimum of one "io".  and then we have some cost per "io" that is
  // a configurable (with different hdd and ssd defaults), and add
  // that to the bytes value.
  uint64_t ios = 0;
#if defined(HAVE_LIBAIO) || defined(HAVE_POSIXAIO)
  for (auto& p : pending_aios) {
    ios += p.iov.size();
  }
#endif
#ifdef HAVE_SPDK
  ios += total_nseg;
#endif
  return ios;
}

void ThIOContext::release_running_aios()
{
  ceph_assert(!num_running);
#if defined(HAVE_LIBAIO) || defined(HAVE_POSIXAIO)
  // release aio contexts (including pinned buffers).
  running_aios.clear();
#endif
}

ThBlockDevice *ThBlockDevice::create(CephContext* cct, const string& path,
    aio_callback_t cb, void *cbpriv, aio_callback_t d_cb, void *d_cbpriv)
{
  string type = "kernel";
  char buf[PATH_MAX + 1];
  int r = ::readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (r >= 0) {
    buf[r] = '\0';
    char *bname = ::basename(buf);
    if (strncmp(bname, SPDK_PREFIX, sizeof(SPDK_PREFIX)-1) == 0)
      type = "ust-nvme";
  }

  dout(1) << __func__ << " path " << path << " type " << type << dendl;

  return new ThKernelDevice(cct, cb, cbpriv, d_cb, d_cbpriv);

}

void ThBlockDevice::queue_reap_ioc(ThIOContext *ioc)
{
  std::lock_guard l(ioc_reap_lock);
  if (ioc_reap_count.load() == 0)
    ++ioc_reap_count;
  ioc_reap_queue.push_back(ioc);
}

void ThBlockDevice::reap_ioc()
{
  if (ioc_reap_count.load()) {
    std::lock_guard l(ioc_reap_lock);
    for (auto p : ioc_reap_queue) {
      dout(20) << __func__ << " reap ioc " << p << dendl;
      delete p;
    }
    ioc_reap_queue.clear();
    --ioc_reap_count;
  }
}
