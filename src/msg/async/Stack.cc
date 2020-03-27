// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 XSky <haomai@xsky.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <mutex>

#include "include/compat.h"
#include "common/Cond.h"
#include "common/errno.h"
#include "PosixStack.h"
#ifdef HAVE_RDMA
#include "rdma/RDMAStack.h"
#endif
#ifdef HAVE_DPDK
#include "dpdk/DPDKStack.h"
#endif

#include "common/dout.h"
#include "include/ceph_assert.h"

#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << "stack "

std::function<void ()> NetworkStack::add_thread(unsigned i)
{
  Worker *w = workers[i];
  // selective dispatch
  w->whoami_name = whoami_name;
  return [this, w]() {
      char tp_name[16];
      sprintf(tp_name, "msgr-worker-%u", w->id);
      ceph_pthread_setname(pthread_self(), tp_name);
      const unsigned EventMaxWaitUs = 30000000;
      w->center.set_owner();
      ldout(cct, 10) << __func__ << " starting" << dendl;

      if (cct->_conf->osd_affinity_enable && cct->_conf->name.is_osd()) {
	int cpu = w->cpu_affinity;
	if (cpu != -1) {
	  cpu_set_t set;
	  CPU_ZERO(&set);
	  CPU_SET(cpu, &set);
  #if 0
	  if (sched_setaffinity(getpid(), sizeof(cpu_set_t), &set)) {
	    ldout(cct, 0) << __func__ << " error occur during configuring affinity " << dendl;
	  }
  #endif
	  int s = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &set);
	  if (s != 0) {
	    ldout(cct, 0) << __func__ << " error occur during configuring affinity " << dendl;
	  }
	  ldout(cct, 0) << __func__ << " this is osd, pin this thread to cpu " << cpu << dendl;
	  //lderr(cct) << __func__ << " this is osd, pin this thread to cpu " << cpu << dendl;
	}

      }
      //ldout(cct, 0) << __func__ << " omw process_events...." << dendl;

      w->initialize();
      w->init_done();
      //lderr(cct) << __func__ << " omw msg thread cpu? " << w->cpu_affinity  << dendl;
      //ldout(cct, 0) << __func__ << " omw process_events...." << dendl;
      while (!w->done) {
        ldout(cct, 30) << __func__ << " calling event process" << dendl;
#if 0
        ldout(cct, 0) << " whoami_name " <<  whoami_name <<  " is_osd " << this->whoami_name.is_osd() 
		      << " worker whoami_name " << w->whoami_name << " is osd " << is_osd << dendl;
        ldout(cct, 0) << " omw debug process_events " << test << " affinity " << w->cpu_affinity << dendl;
        ldout(cct, 0) << " omw debug process_events " <<  " affinity " << w->cpu_affinity 
		      << " cur affinity " << sched_getcpu() << dendl;
        ldout(cct, 0) << " omw debug process_events " <<  " affinity " << w->cpu_affinity 
		      << " cur affinity " << sched_getcpu() << dendl;
#endif

        ceph::timespan dur;
        int r = w->center.process_events(EventMaxWaitUs, &dur);
        if (r < 0) {
          ldout(cct, 20) << __func__ << " process events failed: "
                         << cpp_strerror(errno) << dendl;
          // TODO do something?
        }
        w->perf_logger->tinc(l_msgr_running_total_time, dur);
      }
      w->reset();
      w->destroy();
  };
}

std::shared_ptr<NetworkStack> NetworkStack::create(CephContext *c, const string &t)
{
  if (t == "posix")
    return std::make_shared<PosixNetworkStack>(c, t);
#ifdef HAVE_RDMA
  else if (t == "rdma")
    return std::make_shared<RDMAStack>(c, t);
#endif
#ifdef HAVE_DPDK
  else if (t == "dpdk")
    return std::make_shared<DPDKStack>(c, t);
#endif

  lderr(c) << __func__ << " ms_async_transport_type " << t <<
    " is not supported! " << dendl;
  ceph_abort();
  return nullptr;
}

Worker* NetworkStack::create_worker(CephContext *c, const string &type, unsigned i)
{
  if (type == "posix")
    return new PosixWorker(c, i);
#ifdef HAVE_RDMA
  else if (type == "rdma")
    return new RDMAWorker(c, i);
#endif
#ifdef HAVE_DPDK
  else if (type == "dpdk")
    return new DPDKWorker(c, i);
#endif

  lderr(c) << __func__ << " ms_async_transport_type " << type <<
    " is not supported! " << dendl;
  ceph_abort();
  return nullptr;
}

NetworkStack::NetworkStack(CephContext *c, const string &t): type(t), started(false), cct(c)
{
  ceph_assert(cct->_conf->ms_async_op_threads > 0);

  const int InitEventNumber = 5000;
  num_workers = cct->_conf->ms_async_op_threads;
  if (num_workers >= EventCenter::MAX_EVENTCENTER) {
    ldout(cct, 0) << __func__ << " max thread limit is "
                  << EventCenter::MAX_EVENTCENTER << ", switching to this now. "
                  << "Higher thread values are unnecessary and currently unsupported."
                  << dendl;
    num_workers = EventCenter::MAX_EVENTCENTER;
  }

  for (unsigned i = 0; i < num_workers; ++i) {
    Worker *w = create_worker(cct, type, i);
    if (cct->_conf->osd_affinity_enable) {
      if (cct->_conf->name.is_osd()) {
	string p = cct->_conf.get_sr_config(cct->_conf->osd_data, SD_BASE_CORE, osd_whoami);
	ldout(cct, 0) << __func__ << " network thread sd affinity " << w->cpu_affinity << " p " << p << dendl;
	if (!p.length() || p.empty()) {
	  w->cpu_affinity = -1;
	} else {
	  w->cpu_affinity = std::stoi(p);
	  w->cpu_affinity = w->cpu_affinity + i;
	  ldout(cct, 0) << __func__ << " network thread sd affinity " << w->cpu_affinity << dendl;
    #if 0
	  w->cpu_affinity = (osd_whoami % cct->_conf->osd_per_node) *
				    cct->_conf->osd_threads_sd + i;
    #endif
	}
      }
    }
    w->center.init(InitEventNumber, i, type);
    workers.push_back(w);
  }
}

void NetworkStack::start()
{
  std::unique_lock<decltype(pool_spin)> lk(pool_spin);

  if (started) {
  ldout(cct, 0) << __func__ << " already started ?name " << whoami_name << dendl;
  lderr(cct) << __func__ << " already started ?name " << whoami_name << dendl;
    return ;
  }

  // selective dispath
  ldout(cct, 0) << __func__ << " name " << whoami_name << dendl;
  lderr(cct) << __func__ << " name " << whoami_name << dendl;

  for (unsigned i = 0; i < num_workers; ++i) {
    if (workers[i]->is_init())
      continue;
    std::function<void ()> thread = add_thread(i);
    spawn_worker(i, std::move(thread));
  }
  started = true;
  lk.unlock();

  for (unsigned i = 0; i < num_workers; ++i)
    workers[i]->wait_for_init();
}

Worker* NetworkStack::get_worker()
{
  ldout(cct, 30) << __func__ << dendl;

   // start with some reasonably large number
  unsigned min_load = std::numeric_limits<int>::max();
  Worker* current_best = nullptr;

  pool_spin.lock();
  // find worker with least references
  // tempting case is returning on references == 0, but in reality
  // this will happen so rarely that there's no need for special case.
  for (unsigned i = 0; i < num_workers; ++i) {
    unsigned worker_load = workers[i]->references.load();
    if (worker_load < min_load) {
      current_best = workers[i];
      min_load = worker_load;
    }
  }

  pool_spin.unlock();
  ceph_assert(current_best);
  ++current_best->references;
  return current_best;
}

Worker* NetworkStack::get_worker_for_sd(int &last_alloc_num)
{
  ldout(cct, 30) << __func__ << dendl;

   // start with some reasonably large number
  unsigned min_load = std::numeric_limits<int>::max();
  Worker* current_best = nullptr;

  pool_spin.lock();
  // find worker with least references
  // tempting case is returning on references == 0, but in reality
  // this will happen so rarely that there's no need for special case.
  if (last_alloc_num == -1) {
    last_alloc_num = 0;
  } else {
    last_alloc_num++;
    if (last_alloc_num >= num_workers) {
      last_alloc_num = 0;
    }
  }
  current_best = workers[last_alloc_num];

  pool_spin.unlock();
  ceph_assert(current_best);
  return current_best;
}

void NetworkStack::stop()
{
  std::lock_guard<decltype(pool_spin)> lk(pool_spin);
  for (unsigned i = 0; i < num_workers; ++i) {
    workers[i]->done = true;
    workers[i]->center.wakeup();
    join_worker(i);
  }
  started = false;
}

class C_drain : public EventCallback {
  Mutex drain_lock;
  Cond drain_cond;
  unsigned drain_count;

 public:
  explicit C_drain(size_t c)
      : drain_lock("C_drain::drain_lock"),
        drain_count(c) {}
  void do_request(uint64_t id) override {
    Mutex::Locker l(drain_lock);
    drain_count--;
    if (drain_count == 0) drain_cond.Signal();
  }
  void wait() {
    Mutex::Locker l(drain_lock);
    while (drain_count)
      drain_cond.Wait(drain_lock);
  }
};

void NetworkStack::drain()
{
  ldout(cct, 30) << __func__ << " started." << dendl;
  pthread_t cur = pthread_self();
  pool_spin.lock();
  C_drain drain(num_workers);
  for (unsigned i = 0; i < num_workers; ++i) {
    ceph_assert(cur != workers[i]->center.get_owner());
    workers[i]->center.dispatch_event_external(EventCallbackRef(&drain));
  }
  pool_spin.unlock();
  drain.wait();
  ldout(cct, 30) << __func__ << " end." << dendl;
}
