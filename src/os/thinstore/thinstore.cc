// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <boost/container/flat_set.hpp>

#include "include/cpp-btree/btree_set.h"

#include "thinstore.h"
#include "os/kv.h"
#include "include/compat.h"
#include "include/intarith.h"
#include "include/stringify.h"
#include "include/str_map.h"
#include "include/util.h"
#include "common/errno.h"
#include "common/safe_io.h"
#include "common/PriorityCache.h"
#include "auth/Crypto.h"
#include "common/EventTrace.h"
#include "perfglue/heap_profiler.h"
#include "common/blkdev.h"
#include "common/numa.h"
//#include "os/bluestore/KernelDevice.h"
#include "allocator.h"

#if 0
#include "include/encoding.h"
#include "common/Formatter.h"
#include "common/Checksummer.h"
#endif

#define dout_context cct
#define dout_subsys ceph_subsys_bluestore
//#define dout_subsys ceph_subsys_osd

//#define OBJECT_MAX_SIZE 0xffffffff // 32 bits
#define OBJECT_MAX_SIZE ((uint64_t)32*1024*1024) // 32 bits

#if 0
#define PARTITION_JOURNAL_SIZE ((uint64_t)1024 * 1024 * 1024 * 2)
#define PARTITION_META_SIZE ((uint64_t)1024 * 1024 * 1024 * 16)
#define PARTITION_ALLOC_SIZE ((uint64_t)1024 * 1024 * 1024 * 2)
#endif

/* configure parameters
1. append or overwirte
2. aio or direct
- data, journal, meta
3. batch
- aio or direct
- append 
*/

// ToDo
// conflict table 20200315

enum {
  TH_JOURNAL,
  TH_META,
  TH_ALLOC,
  TH_DATA
};

//mempool::bluestore_cache_other::unordered_map<ghobject_t,ThOnodeRef> thonode_map;

bool is_align(uint64_t size) 
{
  uint32_t remain = size % ALIGN_SIZE;
  if (remain) {
    return false;
  }
  return true;
}

uint64_t align_byte(uint64_t size) 
{
  uint64_t remain = size % ALIGN_BYTE;
  uint64_t new_size = size;
  if (remain) {
    new_size = size - remain;
  }
  return new_size;
}

inline uint64_t get_addr_from_block(uint64_t block)
{
  ceph_assert(BLOCK_SIZE == 4096);
  return (block << 10) << 2;
}

inline uint64_t get_block_from_addr(uint64_t addr)
{
  ceph_assert(BLOCK_SIZE == 4096);
  return (addr >> 10) >> 2;
}

void get_head_body_tail(int & head, int & body, int & tail, uint64_t offset,
			uint64_t length, uint64_t block_size) 
{
  int n_head = (offset % block_size) ? 1 : 0;
  int tmp = length;
  if (block_size - (offset % block_size) != block_size) {
    if (tmp > block_size) {
      tmp = tmp - (block_size - (offset % block_size));
    } 
  }

  bool need_tail = true;
  int n_body = tmp / block_size;
  if (n_body == 0 && tmp > 0) {
    n_body = 1;
    need_tail = false;
  }
  if (tmp < 0) {
    tmp = 0;
  }

  int n_tail = 0;
  if (need_tail) {
    if ((offset % block_size)+length < block_size) {
      n_tail = 0;
    } else {
      n_tail = ((offset+length) % block_size) ? 1 : 0;
    }
  }

  head = n_head;
  body = n_body; 
  tail = n_tail;
}

ThinStore::Collection::Collection(ThinStore *store_, coll_t cid)
  : CollectionImpl(cid), store(store_)
{
}

ThinStore::ThinStore(CephContext *cct, const string& path)
  : ObjectStore(cct, path), bdev(NULL)
{
  //_init_logger();
  //cct->_conf.add_observer(this);
  //set_cache_shards(1);
}

ThinStore::~ThinStore()
{
  //cct->_conf.remove_observer(this);
  //_shutdown_logger();
  dout(0) << __func__ << " shut down... " << dendl;
  ceph_assert(0);
}


int ThinStore::_open_bdev(bool create)
{
  ceph_assert(bdev == NULL);
  string p = path + "/block";

#if 0
  if (whoami == 0) {
    p = cct->_conf->osd0_path;
  }
  else if (whoami == 1) {
    p = cct->_conf->osd1_path;
  }
  else if (whoami == 2) {
    p = cct->_conf->osd2_path;
  }
#endif
  p = cct->_conf.get_sr_config(cct->_conf->osd_data, SD_PATH, whoami);

  dout(0) << __func__ << " open bdev path " << p << " whoami " << whoami << dendl;

  bdev = ThBlockDevice::create(cct, p, NULL, static_cast<void*>(this), NULL, static_cast<void*>(this));
  int r = bdev->open(p);
  if (r < 0)
    goto fail;

  // initialize global block parameters
  //block_size = bdev->get_block_size();
  block_size = BLOCK_SIZE;
  block_mask = ~(block_size - 1);
  block_size_order = ctz(block_size);
  ceph_assert(block_size == 1u << block_size_order);
  dout(0) << __func__ << " thinstore block size " << block_size << dendl;
#if 0
  // and set cache_size based on device type
  r = _set_cache_sizes();
  if (r < 0) {
    goto fail_close;
  }
#endif
  return 0;

 fail_close:
  bdev->close();
 fail:
  delete bdev;
  bdev = NULL;
  dout(0) << __func__ << " fail !!!!!!!!!!!!! " << dendl;
  return r;
}

int ThinStore::_open_path()
{
  // sanity check(s)
  ceph_assert(path_fd < 0);
  path_fd = TEMP_FAILURE_RETRY(::open(path.c_str(), O_DIRECTORY|O_CLOEXEC));
  if (path_fd < 0) {
    int r = -errno;
    derr << __func__ << " unable to open " << path << ": " << cpp_strerror(r)
	 << dendl;
    return r;
  }
  return 0;
}

void ThinStore::_close_path()
{
  VOID_TEMP_FAILURE_RETRY(::close(path_fd));
  path_fd = -1;
  dout(0) << __func__ << " thinstore closed " << dendl;
}

int ThinStore::mount()
{
  dout(1) << __func__ << " path " << path << dendl;

  int r = _open_path();
  if (r < 0)
    return r;

#if 0
  r = _open_fsid(false);
  if (r < 0)
    goto out_path;
#endif

  // Note: when the node is recovered, re-mount occur. To handle this case,
  // _open_bedev is invoked only if _bdev is not null.
  if (bdev == NULL) {
    r = _open_bdev(false);
    if (r < 0)
      goto out_path;
    //goto out_fsid;
  }

  // ToDo: persistent store
  // mkfs whenever osd is starting..
  mkfs();

  return r;
#if 0
 out_fsid:
  _close_fsid();
#endif
 out_path:
  _close_path();
  return r;
}


void ThinStore::_assign_nid(ThTransContext *txc, ThOnodeRef& o, int sd_index)
{
  ceph_assert(o);
  if (o->thonode.nid) {
    ceph_assert(o->exists);
    return;
  }
  uint64_t nid = ++nid_last[sd_index];
  o->thonode.nid = nid;
  if (txc) {
    txc->last_nid = nid;
  }
  o->exists = true;
}

int ThinStore::_write(ThTransContext *txc,
		      ThOnodeRef& o,
		      uint64_t offset, size_t length,
		      bufferlist& bl,
		      uint32_t fadvise_flags,
		      ThIOContext *thioc,
		      int sd_index)
{
  dout(30) << __func__ << " " <<  o->oid
	   << " 0x" << std::hex << offset << "~" << length << std::dec
	   << dendl;
  int r = 0;
  if (offset + length >= OBJECT_MAX_SIZE) {
    r = -E2BIG;
    dout(0) << __func__ << " please check the object size is too big " << dendl;
  } else {
    _assign_nid(txc, o, sd_index);
    //r = _do_write(txc, c, o, offset, length, bl, fadvise_flags);
    r = _do_write(o, offset, length, bl, sd_index, thioc);
    //txc->write_onode(o);
  }
  return r;
}

int ThinStore::_do_journal_write(uint32_t op,
		      ThOnodeRef& o,
		      uint64_t offset, size_t length,
		      bufferlist& bl,
		      uint32_t fadvise_flags,
		      int sd_index,
		      ThIOContext *thioc,
		      vector<uint32_t> *alloc_data_block_no)
{
  uint64_t j_start_offset = part_desc[sd_index].j_start;
  uint64_t seq = part_desc[sd_index].j_desc.cur_seq;
  
  struct ThJournalEntry j_entry(part_desc[sd_index].j_desc.entry_size);

  //th_aio_queue_t::aio_iter iter = thioc->
  if (enable_async_write) {
    auto p = thioc->pending_aios.back();
    if (p.ops.front() == WRITE_OP) {
      j_entry.oid = o->oid.hobj.oid;
      j_entry.nid = o->thonode.nid;
      j_entry.op = op;
      j_entry.offset = p.offset;
      j_entry.length = p.length;
      // ToDo: partial alloc
      j_entry.alloc_no_begin = alloc_data_block_no->front();
      j_entry.alloc_no_end = alloc_data_block_no->back();
    } 
  } else {
    j_entry.oid = o->oid.hobj.oid;
    j_entry.nid = o->thonode.nid;
    j_entry.op = op;
    j_entry.offset = offset;
    j_entry.length = length;
    // ToDo: partial alloc
    j_entry.alloc_no_begin = alloc_data_block_no->front();
    j_entry.alloc_no_end = alloc_data_block_no->back();
  }

  bufferlist write_bl;
  encode(j_entry, write_bl);

  dout(30) << __func__ << " seq " << part_desc[sd_index].j_desc.cur_seq
	  <<  " size " << o->thonode.size
	  << " block size " << block_size 
	  << " write length " << write_bl.length() <<dendl;

  if (!thioc->batch_direct_io) {
    _do_raw_write(j_start_offset + (seq * part_desc[sd_index].j_desc.entry_size),
		  0, 0, write_bl, sd_index, thioc);
  } else {
    thioc->batch_io_obj[thioc->batch_io_index] = o->oid;
    thioc->batch_direct_bl[thioc->batch_io_index].append(write_bl);
  }

  part_desc[sd_index].j_desc.cur_seq++;
  if (part_desc[sd_index].j_desc.cur_seq >= part_desc[sd_index].j_desc.max_entry_size) {
    part_desc[sd_index].j_desc.cur_seq = 0;
  }
  return 1;
}

uint32_t ThinStore::get_thmeta_location(ThOnodeRef& o, int sd_index) 
{
  assert(o);
  dout(30) << __func__ << " sd_index " << sd_index << " oid " << o->oid 
	  << " hash " << o->oid.hobj.get_hash() << " max entry_size " 
	  << part_desc[sd_index].m_desc.max_entry_size << " entry_size "
	  << part_desc[sd_index].m_desc.entry_size << dendl;
  ceph_assert(part_desc[sd_index].m_desc.max_entry_size);
  // ToDo: Is uint32_t enough ?
  uint32_t location = o->oid.hobj.get_hash() % part_desc[sd_index].m_desc.max_entry_size;
  return location;
}

int ThinStore::_do_meta_write(
		      ThOnodeRef& o,
		      int sd_index,
		      ThIOContext *thioc)
{
  uint64_t m_start_offset = part_desc[sd_index].m_start;
  // find the posision
  uint32_t location = get_thmeta_location(o, sd_index);
  
  bufferlist write_bl;
  encode(o->thonode, write_bl);

  dout(30) << __func__ << " seq " << part_desc[sd_index].m_desc.cur_seq
	  << " location " << location <<  " size " << o->thonode.size
	  << " block size " << o->thonode.block_size 
	  << " write bl length " << write_bl.length() << dendl;

  if (!thioc->batch_direct_io) {
    _do_raw_write(m_start_offset + (location * part_desc[sd_index].m_desc.entry_size),
		  0, 0, write_bl, sd_index, thioc);
  } else {
    thioc->batch_io_obj[thioc->batch_io_index] = o->oid;
    thioc->batch_direct_bl[thioc->batch_io_index].append(write_bl);
  }

  part_desc[sd_index].m_desc.cur_seq++;
  if (part_desc[sd_index].m_desc.cur_seq >= part_desc[sd_index].m_desc.max_entry_size) {
    part_desc[sd_index].m_desc.cur_seq = 0;
  }
  o->thonode.need_flush_block_map = false;
  return 1;
}

void ThinStore::_txc_add_transaction(ThTransContext *txc, Transaction *t, int sd_index, ThIOContext *thioc)
{
  Transaction::iterator i = t->begin();

  // disable collection
#if 0
  vector<CollectionRef> cvec(i.colls.size());
  unsigned j = 0;
  for (vector<coll_t>::iterator p = i.colls.begin(); p != i.colls.end();
       ++p, ++j) {
    cvec[j] = _get_collection(*p);
  }
#endif

  vector<ThOnodeRef> ovec(i.objects.size());

  for (int pos = 0; i.have_op(); ++pos) {
    Transaction::Op *op = i.decode_op();
    int r = 0;

    // no coll or obj
    if (op->op == Transaction::OP_NOP)
      continue;

    spg_t pgid;
#if 0
    // collection operations
    //CollectionRef &c = cvec[op->cid];
    // initialize osd_pool_id and do a smoke test that all collections belong
    // to the same pool

    if (!!c ? c->cid.is_pg(&pgid) : false) {
      ceph_assert(txc->osd_pool_id == META_POOL_ID ||
                  txc->osd_pool_id == pgid.pool());
      txc->osd_pool_id = pgid.pool();
    }
#endif

    // these operations implicity create the object
    bool create = false;
    if (op->op == Transaction::OP_TOUCH ||
	op->op == Transaction::OP_CREATE ||
	op->op == Transaction::OP_WRITE ||
	op->op == Transaction::OP_ZERO) {
      create = true;
    }

    // object operations
    ThOnodeRef &o = ovec[op->oid];
    const ghobject_t oid = i.get_oid(op->oid);
	/********************
	********************
	fast test
	********************/
    if (!o) {
      //o = c->get_onode(oid, create, op->op == Transaction::OP_CREATE);
      ThOnodeRef tmp = get_thonode(oid, create, sd_index);
      ovec[op->oid] = tmp;
      o = tmp;
      assert(o);
      o->exists = true;
    }

    if (!create && (!o || !o->exists)) {
      dout(10) << __func__ << " op " << op->op << " got ENOENT on "
	       << i.get_oid(op->oid) << dendl;
      r = -ENOENT;
      goto endop;
    }

    // ceph_assert(o);
    dout(30) << __func__ << " omw test " << o->thonode.cached << " " 
	    << o->thonode.need_flush_block_map << " op " << op->op 
	    << dendl;

    switch (op->op) {
    case Transaction::OP_CREATE:
    case Transaction::OP_TOUCH:
      // ToDo
      //r = _touch(txc, c, o);
      break;

    case Transaction::OP_SETATTRS:
      {
	map<string, bufferptr> aset;
	i.decode_attrset(aset);

	break;
	// ToDo
	{
	  bufferlist tmp_bl;
	  encode(aset, tmp_bl);
	  _journal_write(o, thioc, tmp_bl, txc);
	}
      }
      break;
    case Transaction::OP_OMAP_SETKEYS:
      {
	map<string, bufferlist> aset;
	i.decode_attrset(aset);

	break;
	// ToDo
	{
	  bufferlist tmp_bl;
	  encode(aset, tmp_bl);
	  _journal_write(o, thioc, tmp_bl, txc);
	}
      }
      break;
    case Transaction::OP_OMAP_SETHEADER:
      {
	bufferlist bl;
	i.decode_bl(bl);
      }
      break;

    case Transaction::OP_SETALLOCHINT:
      {
      }
      break;
    case Transaction::OP_TRUNCATE:
      {
	uint64_t off = op->off;
	if (o->thonode.size > off) {
	  o->thonode.size = off;
	}
      }
      break;
    case Transaction::OP_OMAP_RMKEYS:
      {
	bufferlist keys_bl;
	i.decode_keyset_bl(&keys_bl);
      }
      break;
    case Transaction::OP_WRITE:
      {
        uint64_t off = op->off;
        uint64_t len = op->len;
	uint32_t fadvise_flags = i.get_fadvise_flags();
        bufferlist bl;
        i.decode_bl(bl);
	// do data write

	if (journal_mode) {
	  struct ThChunkInfo thchk_info;
	  bool found = false;
	  _assign_nid(txc, o, sd_index);
	  for (auto p : thioc->seg.chunk_infos) {
	    if (p.oid == o->oid.hobj && p.offset == off && p.len == len) {
	      found = true;
	      p.offset = off;
	      p.len = len;
	      p.d_index = thioc->seg.num_chunk;
	      break;
	    }
	  }
	  if (!found) {
	    thchk_info.offset = off;
	    thchk_info.len = len;
	    thchk_info.d_index = thioc->seg.num_chunk;
	    thchk_info.oid = o->oid.hobj;
	    thioc->seg.chunk_infos.push_back(thchk_info);
	    txc->cache_onode[thchk_info.oid] = o;
	  }
	   
	  dout(30) << __func__ << " oid " << o->thonode.oid << " bl blength "
		  << bl.length() << " num chunk " << thioc->seg.num_chunk 
		  << " off " << off << " len " << len << dendl;
	  if (bl.length()) {
	    thioc->seg.chunk_bl.append(bl);
	    thioc->seg.num_chunk++;
	  }
	} else {
	  ceph_assert(bl.length());
#if 0
	  // test performance
	  dout(0) << __func__ << " batch_io_index " << thioc->batch_io_index 
		  << " sd_index " << sd_index << " bl " << bl.length() 
		  << " oid " << op->oid << dendl;

	  thioc->batch_direct_bl.push_back(bl);
	  thioc->batch_direct_bl_offset.push_back(
		  part_desc[sd_index].d_start + 
		  (block_size * thioc->batch_io_index) + 
		  block_size);
	  thioc->batch_direct_bl.push_back(bl);
	  thioc->batch_direct_bl_offset.push_back(
		  part_desc[sd_index].d_start);
#endif
	  
	  r = _write(txc, o, off, len, bl, fadvise_flags, thioc, sd_index);
	}
	assert(r >= 0);
      }
      break;
    default:
      // ToDo
      dout(0) << __func__ << " not support op " << op->op << dendl;
    }

  endop:
    if (r < 0) {
      bool ok = false;
    }

  }
}

bool ThinStore::is_new_thonode(ThOnodeRef o)
{
  return (o->thonode.block_size == 0 && o->thonode.size == 0);
}

void ThinStore::write_thonode(ThOnodeRef o, int sd_index)
{
  uint32_t location = get_thmeta_location(o, sd_index);
  uint64_t m_start_offset = part_desc[sd_index].m_start;
  bufferlist bl;

  uint64_t write_size = (block_size > part_desc[sd_index].m_desc.entry_size) ? 
			block_size : part_desc[sd_index].m_desc.entry_size;

  encode(o->thonode, bl);

  int r = bdev->write(m_start_offset + (part_desc[sd_index].m_desc.entry_size * location), 
		      bl, false);
  if (r < 0) { 
    dout(0) << __func__ << " error " << r << " start_offset " 
	    << m_start_offset << " location "
	    << location << " sd_index " << sd_index  
	    << " block size " << block_size 
	    << " entry size " << part_desc[sd_index].m_desc.entry_size
	    << " location " << location
	    << dendl;
    ceph_assert(0);
  }
  return;
}

void ThinStore::read_thonode(ThOnodeRef o, const ghobject_t& oid, int sd_index)
{
  // find location
  uint32_t location = get_thmeta_location(o, sd_index);
  uint64_t m_start_offset = part_desc[sd_index].m_start;
  bufferlist bl;
  // ToDo: the read depending on the block size
#if 0
  dout(30) << __func__ << " start_offset " << m_start_offset << " location "
	  << location << " sd_index " << sd_index  
	  << " block size " << block_size 
	  << " entry size " << part_desc[sd_index].m_desc.entry_size
	  << dendl;
#endif

  // ToDo: how to check the object is stored 
  uint64_t read_size = (block_size > part_desc[sd_index].m_desc.entry_size) ? 
			block_size : part_desc[sd_index].m_desc.entry_size;
  int r = bdev->read(m_start_offset + (part_desc[sd_index].m_desc.entry_size * location), 
		    read_size, &bl, NULL,
		    false);
  if (r < 0) { 
    dout(0) << __func__ << " error " << r << " start_offset " 
	    << m_start_offset << " location "
	    << location << " sd_index " << sd_index  
	    << " block size " << block_size 
	    << " entry size " << part_desc[sd_index].m_desc.entry_size
	    << " location " << location
	    << dendl;
    ceph_assert(0);
  }
  ceph_assert(bl.length() == read_size);
  // ToDo multiple block map
  if (!o->thonode.block_maps) {
    o->thonode.block_maps = new ThMetaBlockMap(block_size);
  }
  o->thonode.aligned_size = part_desc[sd_index].m_desc.entry_size;
  auto p = bl.cbegin();
  decode(o->thonode, p);
  if (!is_new_thonode(o)) {
    dout(0) << __func__ << " check oid conflict ! " << dendl;
  }
  o->thonode.cached = true;
  o->thonode.need_flush_block_map = true;
  o->thonode.block_size = block_size;
  o->thonode.has_block_map = true;
  o->thonode.oid = oid.hobj;
}

// for in-place update
#if 0
void ThinStore::read_thonode(ThOnodeRef o, const ghobject_t& oid, int sd_index)
{
  // find location
  uint32_t location = get_thmeta_location(o, sd_index);
  uint64_t m_start_offset = part_desc[sd_index].m_start;
  bufferlist bl;
  // ToDo: the read depending on the block size
  uint32_t read_entry = block_size / part_desc[sd_index].m_desc.entry_size;

#if 0
  dout(0) << __func__ << " start_offset " << m_start_offset << " location "
	  << location << " sd_index " << sd_index << " read entry " 
	  << read_entry << " block size " << block_size 
	  << " entry size " << part_desc[sd_index].m_desc.entry_size
	  << dendl;
#endif

  int r = bdev->read(m_start_offset + (part_desc[sd_index].m_desc.entry_size * location), 
		    block_size, &bl, NULL,
		    false);
  if (r < 0) { 
    dout(0) << __func__ << " error " << r << dendl;
  }
  ceph_assert(bl.length() == block_size);
  // ToDo multiple block map
  if (!o->thonode.block_maps) {
    o->thonode.block_maps = new ThMetaBlockMap;
    if (block_pre_allocation) {
      vector<uint32_t> block_no = 
	(alloc[sd_index])->alloc_data_block(NUMBER_OF_BLOCK_MAPS, sd_index);
      ceph_assert(block_no.size() >= NUMBER_OF_BLOCK_MAPS);
      for (uint32_t i = 0; i < NUMBER_OF_BLOCK_MAPS; i++) {
	o->thonode.block_maps->block_maps[i] = block_no[i];
      }
    } else if (block_pre_allocation_4mb) {
      uint32_t need_block_num = ((1024*1024*4)/block_size)*2;
      //dout(0) << __func__ << " need_block_num " << need_block_num << dendl;
      vector<uint32_t> block_no = 
	(alloc[sd_index])->alloc_data_block(need_block_num, sd_index);
      //dout(0) << __func__ << " need_block_num " << need_block_num << dendl;
      ceph_assert(block_no.size() >= need_block_num);
      for (uint32_t i = 0; i < need_block_num; i++) {
	o->thonode.block_maps->block_maps[i] = block_no[i];
      }
    }
  }
  o->thonode.aligned_size = part_desc[sd_index].m_desc.entry_size;
  auto p = bl.cbegin();
  decode(o->thonode, p);
  o->thonode.cached = true;
  o->thonode.need_flush_block_map = true;
  o->thonode.block_size = block_size;
  if (block_pre_allocation_4mb || block_pre_allocation) {
    o->thonode.has_block_map = true;
  }
}
  dout(0) << __func__ << " load thonde, try to make cache " << dendl;

  // load meta entry as much as it read 
  for (int i = 1; i < read_entry; i++) {
    ThOnode * t = new ThOnode;
    decode(t->thonode, bl);
    if (t->thonode.size != 0 && t->thonode.block_size != 0 && t->thonode.nid !=0 ) {
      ThOnodeRef o;
      ghobject_t ghobj_t = ghobject_t(t->thonode.oid, 
			      ghobject_t::NO_GEN, shard_id_t::NO_SHARD);
      o.reset(t);
      //thonode_map[sd_index].at(ghobj_t) = o;
      thonode_map[sd_index].insert( make_pair(ghobj_t, o) ); //= o;
      dout(0) << __func__ << " put read onode on the cache " << t->thonode.oid 
	      << dendl;
    } else {
      delete t;
    }
  }
#endif

// cache ThOnode
ThOnodeRef ThinStore::get_thonode(
  const ghobject_t& oid,
  bool create,
  int sd_index) 
{
  ceph_assert(thonode_map.size());
  //ceph::unordered_map<ghobject_t, ThOnodeRef>::iterator p  = thonode_map[sd_index].find(oid);
  ceph::unordered_map<hobject_t, ThOnodeRef>::iterator p; // = thonode_map[sd_index].find(oid);
  bool found = false;
  if (!thonode_map[sd_index]->empty()) {
    p = thonode_map[sd_index]->find(oid.hobj);
    if (thonode_map[sd_index]->end() != p) {
      found = true;
    }
  }
  ThOnodeRef o;
  //if (thonode_map[sd_index].end() != p) {
  if (found) {
    o = p->second;
    if (o) {
      return o;
    } else {
      ceph_assert(0 == "this shouldn't be null");
    }
  }

  ThOnode * t = new ThOnode(oid);
  assert(t);
  o.reset(t);
  thonode_map[sd_index]->insert( make_pair(oid.hobj, o) ); //= o;
  read_thonode(o, oid, sd_index);

  return o;
}

void ThinStore::_journal_write(ThOnodeRef o, ThIOContext * thioc, bufferlist &bl, ThTransContext * txc)
{
  struct ThChunkInfo thchk_info;
  thchk_info.offset = 0;
  thchk_info.len = 0;
  thchk_info.d_index = thioc->seg.num_chunk;
  thchk_info.oid = o->oid.hobj;
  thioc->seg.chunk_infos.push_back(thchk_info);
  txc->cache_onode[thchk_info.oid] = o;
   
  dout(30) << __func__ << " oid " << o->thonode.oid << " bl blength "
	  << bl.length() << " num chunk " << thioc->seg.num_chunk 
	  << " len " << bl.length() << dendl;
  if (bl.length()) {
    thioc->seg.chunk_bl.append(bl);
    thioc->seg.num_chunk++;
  }
}

int ThinStore::do_journal_write(int sd_index, ThIOContext * thioc, ThTransContext & txc) 
{
  uint64_t start_addr = part_desc[sd_index].d_desc.journal_pointer + 
			thioc->seg.chunk_start;
  bufferlist bl;
  encode(thioc->seg, bl);
  // dout(0) << " segment: " << thioc->seg << dendl;

  // find a suitable allocation
  // find_alloc_pointer
  // need check
  // need check
  // need check
  // need check
  // need check
  // need check
  if (part_desc[sd_index].d_desc.journal_pointer + bl.length() >
      part_desc[sd_index].d_start + part_desc[sd_index].d_len) {

    dout(0) << __func__ << " journal is full " 
	    << part_desc[sd_index].d_desc.journal_pointer
	    << dendl;
    part_desc[sd_index].d_desc.journal_pointer =
		      part_desc[sd_index].d_start;
    dout(0) << __func__ << " journal is reset " 
	    << part_desc[sd_index].d_desc.journal_pointer
	    << dendl;
  }

  dout(30) << __func__ << " write data part offset " 
	  << part_desc[sd_index].d_desc.journal_pointer 
	  << " data len " << bl.length() << dendl;
  _do_raw_write(part_desc[sd_index].d_desc.journal_pointer, 
		bl.length(),
		0, bl, sd_index, thioc);

    
  for (auto p : thioc->seg.chunk_infos) {
    uint64_t cur = 0;
    uint64_t offset = p.offset;
    uint64_t cur_offset = offset;
    uint32_t block_off = offset / block_size;
    ThOnodeRef o = txc.cache_onode[p.oid];
    ceph_assert(o);
    if (p.d_index != -1) {
      // found offset
      // must align !
      // update the block location
      while (cur < p.len) {
	ceph_assert((offset % block_size) == 0);
	ceph_assert(o->thonode.block_maps);
	o->thonode.block_maps->set_addr_by_offset(
		      cur_offset, 
		      start_addr + 
		      cur);
	dout(30) << __func__ << " update block_maps->block_offset "
		<< block_off << " cur " << cur << " cur_offset " 
		<< cur_offset << " addr " 
		<< start_addr + cur
		<< dendl;
	cur += block_size;
	cur_offset += block_size;
	block_off = cur_offset / block_size;
      }
      start_addr += cur;
    }

    if (p.m_index != -1) {
      o->thonode.mattrs_seg_id = thioc->seg.seg_id;
    }
#if 0
    // update size
    if (p.len + p.offset > o->thonode.size) {
      o->thonode.size = p.len + p.offset;
    }
#endif
  }


  // need a persisten state
  part_desc[sd_index].seg_location[thioc->seg.seg_id] = part_desc[sd_index].d_desc.journal_pointer;
  part_desc[sd_index].d_desc.journal_pointer += bl.length();

  return 0;
}

// default mode is in-place mode
int ThinStore::queue_transactions(
  CollectionHandle& ch,
  vector<struct sd_entry *> ops_list,
  int sd_index)
{
  list<Context *> on_applied, on_commit, on_applied_sync;
  for (auto p : ops_list) {
    ObjectStore::Transaction::collect_contexts(
      p->tls, &on_applied, &on_commit, &on_applied_sync);
  }
  Collection *c = static_cast<Collection*>(ch.get());
  auto start = mono_clock::now();
  
  /* if journal_mode is false */
  if (journal_mode == false) {
    return queue_transactions_in_place(ch, ops_list, sd_index, false);
  }

  dout(10) << __func__ << " thin store batching " << " sd_index " << sd_index 
	  << " ops list size " << ops_list.size() << dendl;

  ceph_assert(sd_index >= 0);

  ThIOContext *thioc = new ThIOContext(cct, NULL, false);
  ThTransContext txc;

  thioc->batch_direct_io = true;

  // do write
  for (auto index : ops_list) {
    if (journal_mode) {
      if (thioc->seg.seg_id) {
	thioc->seg.seg_id = g_seg_id++;
      }
    }
    for (vector<Transaction>::iterator p = index->tls.begin(); p != index->tls.end(); 
	  ++p) {
      OpRequestRef op = index->op;
      ceph_assert(op);
      //dout(0) << __func__ << " type " << index->type << dendl;
      _txc_add_transaction(&txc, &(*p), op->sd_index, thioc);
#if 0
      dout(0) << __func__ << " num ojbet " << thioc->seg.num_objects 
	      << " chunk info size " << thioc->seg.chunk_infos.size() << dendl;
#endif
    }
  }
  thioc->seg.num_objects = thioc->seg.chunk_infos.size();


  // ToDo: Except OP_WRITE, what operations is to be handled?
  // ceph_assert(thioc->has_pending_aios());
  // do actual write
  if (thioc->batch_direct_io) {
    // ToDo: fixed length
#if 0
    dout(0) << __func__ << " do batch io " << thioc->batch_direct_bl_offset[0]
	    << " length " << thioc->batch_direct_bl[0].length() 
	    << dendl;
#endif

    bufferlist bl;
    encode(thioc->seg, bl);
    // dout(0) << " segment: " << thioc->seg << dendl;

    // find a suitable allocation
    // find_alloc_pointer
    // need check
    // need check
    // need check
    // need check
    // need check
    // need check
    if (part_desc[sd_index].d_desc.journal_pointer + bl.length() >
	part_desc[sd_index].d_start + part_desc[sd_index].d_len) {

      dout(0) << __func__ << " journal is full " 
	      << part_desc[sd_index].d_desc.journal_pointer
	      << dendl;
      part_desc[sd_index].d_desc.journal_pointer =
			part_desc[sd_index].d_start;
      dout(0) << __func__ << " journal is reset " 
	      << part_desc[sd_index].d_desc.journal_pointer
	      << dendl;
    }

    dout(30) << __func__ << " write data part offset " 
	    << part_desc[sd_index].d_desc.journal_pointer 
	    << " data len " << bl.length() << dendl;
    _do_raw_write(part_desc[sd_index].d_desc.journal_pointer, 
		  bl.length(),
		  0, bl, sd_index, thioc);

    
    uint64_t start_addr = part_desc[sd_index].d_desc.journal_pointer + 
			  thioc->seg.chunk_start;
    for (auto p : thioc->seg.chunk_infos) {
      uint64_t cur = 0;
      uint64_t offset = p.offset;
      uint64_t cur_offset = offset;
      uint32_t block_off = offset / block_size;
      ThOnodeRef o = txc.cache_onode[p.oid];
      ceph_assert(o);
      if (p.d_index != -1) {
	// found offset
	// must align !
	// update the block location
	while (cur < p.len) {
	  ceph_assert((offset % block_size) == 0);
	  ceph_assert(o->thonode.block_maps);
	  o->thonode.block_maps->set_addr_by_offset(
			cur_offset, 
			start_addr + 
			cur);
	  dout(30) << __func__ << " update block_maps->block_offset "
		  << block_off << " cur " << cur << " cur_offset " 
		  << cur_offset << " addr " 
		  << start_addr + cur
		  << dendl;
	  cur += block_size;
	  cur_offset += block_size;
	  block_off = cur_offset / block_size;
	}
	start_addr += cur;
      }

      if (p.m_index != -1) {
	o->thonode.mattrs_seg_id = thioc->seg.seg_id;
      }
      // update size
      if (p.len + p.offset > o->thonode.size) {
	o->thonode.size = p.len + p.offset;
      }
    }


    // need a persisten state
    part_desc[sd_index].seg_location[thioc->seg.seg_id] = part_desc[sd_index].d_desc.journal_pointer;
    part_desc[sd_index].d_desc.journal_pointer += bl.length();
    

#if 0
    _do_raw_write(thioc->batch_direct_bl_offset[0], 
		  thioc->batch_direct_bl[0].length(),
		  0, thioc->batch_direct_bl[0], sd_index, thioc);
    // aio
    int r = bdev->aio_write(thioc->batch_direct_bl_offset[0],
			    thioc->batch_direct_bl[0], thioc, false, 0);
    if (r < 0) {
      dout(0) << __func__ << " error: " << r << dendl;
      assert(0);
    }
#endif
  }
  if (thioc->has_pending_aios()) {
    bdev->aio_submit(thioc);
    thioc->aio_wait();
  }

  for (auto c : on_applied_sync) {                                                      
    c->complete(0);                                                                     
  }
  if (!on_applied.empty()) {                                                            
    if (c->commit_queue) {
      c->commit_queue->queue(on_applied);                                               
    } else {
      //finisher.queue(on_applied);                                                       
      for (auto cc : on_applied) {                                                      
	cc->complete(0);                                                                     
      }
    }                                                                                   
  } 

#if 0
  dout(30) << __func__ << " end " << mono_clock::now() - start << dendl;
#endif
  if (thioc)
    delete thioc;

  return 0;
}

int ThinStore::queue_transactions_in_place(
  CollectionHandle& ch,
  vector<struct sd_entry *> ops_list,
  int sd_index,
  bool journal_mode)
{
  list<Context *> on_applied, on_commit, on_applied_sync;
  for (auto p : ops_list) {
    ObjectStore::Transaction::collect_contexts(
      p->tls, &on_applied, &on_commit, &on_applied_sync);
  }
  Collection *c = static_cast<Collection*>(ch.get());
  auto start = mono_clock::now();
  
  /* if journal_mode is false */
  dout(30) << __func__ << " thin store batching " << " sd_index " 
	  << sd_index << " ops list size " << ops_list.size() 
	  << dendl;

  ceph_assert(sd_index >= 0);

  ThIOContext *thioc = new ThIOContext(cct, NULL, false);
  ThTransContext txc;

  if (!enable_async_write) {
    if (journal_mode == false) {
      thioc->batch_direct_io = true;
      thioc->batch_io_size = ops_list.size();
    } else {
      thioc->batch_direct_io = true;
      thioc->batch_io_size = 1;
      if (!thioc->batch_direct_bl.size()) {
	bufferlist bl;
	ghobject_t tmp;
	thioc->batch_direct_bl.push_back(bl);
	thioc->batch_io_obj.push_back(tmp);
	thioc->batch_direct_bl_offset.push_back(0);
      }
    }
  }

  // do write
  for (auto index : ops_list) {
    for (vector<Transaction>::iterator p = index->tls.begin(); p != index->tls.end(); 
	  ++p) {
      OpRequestRef op = index->op;
      ceph_assert(op);

      //dout(0) << __func__ << " type " << index->type << dendl;
      if (enable_async_write) {
	_txc_add_transaction(&txc, &(*p), op->sd_index, thioc);
      } else {
	_txc_add_transaction(&txc, &(*p), op->sd_index, thioc);
	thioc->batch_io_index++;
      }
    }
  }

  ceph_assert(ops_list.size() == thioc->batch_io_index);

  // ToDo: Except OP_WRITE, what operations is to be handled?
  // ceph_assert(thioc->has_pending_aios());
  // do actual write
  if (thioc->batch_direct_io) {
    // ToDo: fixed length
    for (int i = 0; i < thioc->batch_io_index; i++) {
#if 0
      dout(0) << __func__ << " do batch io " << thioc->batch_direct_bl_offset[i]
	      << " length " << thioc->batch_direct_bl[i].length() 
	      << dendl;
#endif
      _do_raw_write(thioc->batch_direct_bl_offset[i], 
		    thioc->batch_direct_bl[i].length(),
		    0, thioc->batch_direct_bl[i], sd_index, thioc);
    }

#if 0
    // aio
    int r = bdev->aio_write(thioc->batch_direct_bl_offset[0],
			    thioc->batch_direct_bl[0], thioc, false, 0);
    if (r < 0) {
      dout(0) << __func__ << " error: " << r << dendl;
      assert(0);
    }
#endif
    if (!thioc->seg.chunk_infos.empty()) {
      do_journal_write(sd_index, thioc, txc);
    }
  }
  if (thioc->has_pending_aios()) {
    bdev->aio_submit(thioc);
    thioc->aio_wait();
  }


  for (auto c : on_applied_sync) {                                                      
    c->complete(0);                                                                     
  }
  if (!on_applied.empty()) {                                                            
    if (c->commit_queue) {
      c->commit_queue->queue(on_applied);                                               
    } else {
      //finisher.queue(on_applied);                                                       
      for (auto cc : on_applied) {                                                      
	cc->complete(0);                                                                     
      }
    }                                                                                   
  } 

#if 0
  dout(0) << __func__ << " end " << mono_clock::now() - start << dendl;
#endif
  if (thioc)
    delete thioc;

  return 0;
}

int ThinStore::queue_transactions(
  CollectionHandle& ch,
  vector<Transaction>& tls,
  TrackedOpRef op,
  ThreadPool::TPHandle *handle,
  int sd_index)
{
  list<Context *> on_applied, on_commit, on_applied_sync;
  ObjectStore::Transaction::collect_contexts(
    tls, &on_applied, &on_commit, &on_applied_sync);
  // ToDo: do we need to handle on_commit
  auto start = mono_clock::now();
  Collection *c = static_cast<Collection*>(ch.get());
  dout(0) << __func__ << " thin store " << " sd_index " << sd_index << dendl;

  // create IO context
  ThIOContext *thioc = new ThIOContext(cct, NULL, false);
  //ThTransContext * txc = new ThTransContext;
  ThTransContext * txc = NULL;
  // do write
  for (vector<Transaction>::iterator p = tls.begin(); p != tls.end(); ++p) {
    //txc->bytes += (*p).get_num_bytes();
    _txc_add_transaction(txc, &(*p), op->sd_index, thioc);
  }

  // ToDo: Except OP_WRITE, what operations is to be handled?
  // ceph_assert(thioc->has_pending_aios());
  // do actual write
  if (thioc->has_pending_aios()) {
    bdev->aio_submit(thioc);
    thioc->aio_wait();
  }

  /******************************************************
  ******************************************************
  wait for flush
  *///////////////////////////////

  //int run_aios = thioc->running_aios.size();
#if 0
  ret = bdev->_wait_all_completed(thioc.running_aios.size());
  assert(run_aios == ret);
#endif
  
  // do we need this???
#if 0
  _txc_finish_io(txc);
#endif

  for (auto c : on_applied_sync) {                                                      
    c->complete(0);                                                                     
  }
  if (!on_applied.empty()) {                                                            
    if (c->commit_queue) {
      c->commit_queue->queue(on_applied);                                               
    } else {
      //finisher.queue(on_applied);                                                       
      for (auto cc : on_applied) {                                                      
	cc->complete(0);                                                                     
      }
    }                                                                                   
  } 
  dout(0) << __func__ << " end " << mono_clock::now() - start << dendl;
  if (thioc)
    delete thioc;

  return 0;
}

int ThinStore::_do_write(ThOnodeRef& o, uint64_t offset, size_t length, bufferlist& bl, 
			 int sd_index, ThIOContext *thioc)
{
  uint64_t remain_off = (offset % block_size);
  int n_head = (remain_off) ? 1 : 0;
  int tmp = bl.length();

  bool need_tail = true;
  ceph_assert(block_size == 4096);
  //int n_body = tmp / block_size;
  //int n_body = (tmp >> 10) >> 2;
  int n_body = get_block_from_addr(tmp);
  int n_tail = 0;

  if (block_size - (remain_off) != block_size) {
    if (tmp > block_size) {
      tmp = tmp - (block_size - (offset % block_size));
    } 
  }

  if (n_body == 0 && tmp > 0) {
    n_body = 1;
    need_tail = false;
  }
  if (tmp < 0) {
    tmp = 0;
  }

  if (need_tail) {
    if ((remain_off)+length < block_size) {
      n_tail = 0;
    } else {
      n_tail = ((offset+length) % block_size) ? 1 : 0;
    }
  }
  uint32_t total_block_len = n_head + n_body + n_tail;
  uint32_t allocated_block = 0;

  ceph_assert(total_block_len);

  dout(30) << __func__ << " start === " << " oid " << o->oid <<  " n_head " 
	  << n_head << " n_body " << n_body << " n_tail " << n_tail 
	  << " bl.length " << bl.length() << " block size " << block_size 
	  << " length " << length << " sd_index " 
	  << sd_index << " offset " << offset << " reamin_off " << remain_off
	  << dendl;

  // check if blocks are allocated
  //uint32_t block_start = offset / block_size;
  uint32_t block_start = get_block_from_addr(offset);
  //uint64_t block_start = remain_off;
  // optimization
  if (length < block_size && total_block_len <= 2) {
    // no preallocation
    //total_block_len = total_block_len;
    total_block_len = DEFAULT_ALLOC_BLOCK;
  } else if (total_block_len < DEFAULT_ALLOC_BLOCK) {
    total_block_len = DEFAULT_ALLOC_BLOCK;
  }
  if (block_start + total_block_len > NUMBER_OF_BLOCK_MAPS) {
    total_block_len = NUMBER_OF_BLOCK_MAPS - block_start;
  }

  dout(30) << __func__ << " block start " << block_start << " total block len " 
	  << total_block_len << dendl;
  // temporaly disable

  // ToDo: allocate multiple blocks
  vector<uint32_t> allocated_block_no;
  uint64_t block_start_backup = block_start;

  // tweak!!
  if (offset + length <= (4*1024*1024)) {
    block_start = 0;
  }

  if (total_block_len == DEFAULT_ALLOC_BLOCK) {
    bool get_addr_result;
    uint64_t start_addr = 
	  //o->thonode.block_maps->get_addr_by_offset(offset, get_addr_result);
	  o->thonode.block_maps->get_addr_by_block_no(block_start, get_addr_result);

    if (offset + length > (4*1024*1024)) {
      if (o->thonode.size == 0 && start_addr != 0) {
	ceph_assert(0);
      }
    }


    if (start_addr == 0) {
      vector<uint32_t> block_no = 
	    (alloc[sd_index])->alloc_data_block(total_block_len, sd_index);

      // block_no[0] --> first allocated block_no
      // block_no[1] --> allocated the number of block
      // block_no[2] --> allocated the length
      uint64_t cur = part_desc[sd_index].d_start + get_addr_from_block(block_no[0]);
      for (uint32_t i = block_start; i < block_start + total_block_len; i++) {
	o->thonode.block_maps->set_addr_by_block_no(
				i, 
				cur
				);
	cur += block_size;
#if 0
	//o->thonode.block_maps->block_offset[i] = block_no[i];
	o->thonode.block_maps->set_addr_by_offset(
				offset + ((i - block_start) * block_size), 
				part_desc[sd_index].d_start + (block_size * 
				block_no[i])
				);
	o->thonode.block_maps->set_addr_by_block_no(
				i, 
				part_desc[sd_index].d_start + 
				((block_no[i] << 10) << 2)
				);
#endif

      }
      allocated_block = block_no[1];
      ceph_assert(allocated_block == total_block_len);

      //if (allocated_block && (o->thonode.size < (offset + ((allocated_block << 10) << 2)))) {
      if (allocated_block && (o->thonode.size < offset+length)) {
	o->thonode.size = offset + length;
      } else if (allocated_block && (o->thonode.size < 
	  get_addr_from_block(block_start) + get_addr_from_block(allocated_block)) ) {
	o->thonode.size = get_addr_from_block(block_start) + get_addr_from_block(allocated_block);
      }
    } 

    // block is already allocated, but thonode.size is smaller than allocation size

  } else {
    for (uint32_t i = block_start; i < block_start + total_block_len; i++) {
    bool get_addr_result;
    uint64_t start_addr = 
	  o->thonode.block_maps->get_addr_by_block_no(block_start, get_addr_result);
      //if (o->thonode.block_maps->block_offset[i] == 0) {
      if (start_addr == 0) {
	vector<uint32_t> block_no = (alloc[sd_index])->alloc_data_block(1, sd_index);
	ceph_assert(block_no.size());
	//o->thonode.block_maps->block_offset[i] = block_no[0];
	o->thonode.block_maps->set_addr_by_offset(
				offset + ((i - block_start) * block_size), 
				part_desc[sd_index].d_start + (block_size * 
				block_no[0])
				);
	dout(30) << __func__ << " oid " << o->oid << " block_map index " << i << " allocated block_no " 
		<< block_no[0] << " ori offset " << offset << " block start " << block_start
		<< " total block len " << total_block_len << dendl;
	allocated_block_no.push_back(block_no[0]);
	allocated_block = block_no.size();
      }
    }
  }

  block_start = block_start_backup;


  // find location
  bool get_addr_result;
  // start aligned offset
#if 0
  uint64_t d_start_offset = 
	    o->thonode.block_maps->get_addr_by_offset(offset, get_addr_result);
#endif
  uint64_t d_start_offset = 
	    o->thonode.block_maps->get_addr_by_block_no(
		block_start, get_addr_result);

  if (d_start_offset < part_desc[sd_index].d_start ||
      d_start_offset > (part_desc[sd_index].d_start + part_desc[sd_index].d_len)) {
    dout(0) << __func__ << " start === " << " oid " << o->oid <<  " n_head " 
	    << n_head << " n_body " << n_body << " n_tail " << n_tail 
	    << " bl.length " << bl.length() << " block size " << block_size 
	    << " length " << length << " sd_index " 
	    << sd_index << " offset " << offset << " reamin_off " << remain_off
	    << " allocateed_block " << allocated_block << " total block len " 
	    << total_block_len << " thonode size " << o->thonode.size << dendl;
    ceph_assert(0);
  }

  // ToDo: test cache and..
  // Write amplification...
  dout(30) << __func__ << " d_start " << part_desc[sd_index].d_start 
	  << " block_size " << block_size << " addr " 
	  << d_start_offset << " oid " << o->oid << " offset " << offset
	  << " length " << length << " thonode size " << o->thonode.size 
	  << dendl;
   
  bool aligned = true;
  bufferlist unaligned_buf;
  if (n_head || n_tail) {
    dout(0) << __func__ << " warning unaligned write " << dendl;
    uint64_t align_start_addr =
	     o->thonode.block_maps->get_addr_by_offset(offset, get_addr_result);
    bdev->read(align_start_addr,
	      (n_head + n_body + n_tail) * block_size,
	      &unaligned_buf, NULL, false);
    unaligned_buf.copy_in(remain_off, length, bl.c_str());
    aligned = false;

  }
  // data write
  if (!thioc->batch_direct_io) {
    if (aligned) {
      _do_raw_write(d_start_offset, length,
		    0, bl, sd_index, thioc);
    } else {
      _do_raw_write(d_start_offset, length,
		    0, unaligned_buf, sd_index, thioc);
    }
  } else {
    if (aligned) {
      //thioc->batch_io_obj[thioc->batch_io_index] = o->oid;
      //thioc->batch_io_obj.push_back(o->oid);
      //thioc->batch_direct_bl[thioc->batch_io_index].append(bl);
      //thioc->batch_direct_bl_offset[thioc->batch_io_index] = d_start_offset;


      //thioc->batch_io_obj.push_back(o->oid);
      thioc->batch_direct_bl.push_back(bl);
      thioc->batch_direct_bl_offset.push_back(d_start_offset);
    } else {
      thioc->batch_io_obj.push_back(o->oid);
      thioc->batch_direct_bl.push_back(unaligned_buf);
      thioc->batch_direct_bl_offset.push_back(d_start_offset);
    }
  }

  // delay this
  if (o->thonode.size < offset + length) {
    o->thonode.size = offset + length;
  }


  if (o->thonode.size != (4*1024*1024)) {
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
    // need check !!!33
#if 0
    dout(0) << __func__ << " is not 4MB !!! start === " << " oid " << o->oid <<  " n_head " 
	    << n_head << " n_body " << n_body << " n_tail " << n_tail 
	    << " bl.length " << bl.length() << " block size " << block_size 
	    << " length " << length << " sd_index " 
	    << sd_index << " offset " << offset << " reamin_off " << remain_off
	    << " allocateed_block " << allocated_block << " total block len " 
	    << total_block_len << " thonode size " << o->thonode.size << dendl;
#endif

  }

  // journal write
#if 0
  _do_journal_write(ceph::os::Transaction::OP_WRITE, o, offset, (size_t)bl.length(),
		bl, 0, sd_index, thioc, &allocated_block_no);
#endif

  // meta write
  // this can be delayed
  // ToDo: delayed meta write
#if 0
  _do_meta_write(o, sd_index, thioc);
#endif

  return 0;
}

#if 0
  uint64_t d_start_offset = 
	part_desc[sd_index].d_start + (block_size * 
	o->thonode.block_maps->block_offset[block_start]) + 
	(offset % block_size);
#endif

void ThinStore::_do_raw_write(uint64_t offset, uint64_t length, uint64_t bl_offset,
			       bufferlist& bl, uint32_t sd_index, ThIOContext *thioc)
{
  //ToDo: SPDK 
  dout(30) << __func__ << " offset " << offset << " length " << bl.length() 
	  << " bl_offset " << bl_offset << dendl;
  if (offset + bl.length() > part_desc[sd_index].d_start + part_desc[sd_index].d_len) {
    dout(0) << __func__ << " offset " << offset << " length " << bl.length() 
	    << " bl_offset " << bl_offset << dendl;
    ceph_assert(0);
  }
  if (!bl.length()) {
    dout(0) << __func__ << " offset " << offset << " length " << bl.length() 
	    << " bl_offset " << bl_offset << dendl;
    ceph_assert(0);
  }
  if (enable_async_write) {
    int r = bdev->aio_write(offset, bl, thioc, false, 0);
    if (r < 0) {
      dout(0) << __func__ << " error: " << r << dendl;
      assert(0);
    }
  } else {
    int r = bdev->write(offset, bl, false);
    if (r < 0) {
      dout(0) << __func__ << " error: " << r << dendl;
      assert(0);
    }
  }
}

// make new interfaces (queue_transaction, read)
int ThinStore::read(
  CollectionHandle &c_,
  const ghobject_t& oid,
  uint64_t offset,
  size_t length,
  bufferlist& bl,
  uint32_t op_flags,
  int sd_index)
{
  //auto start = mono_clock::now();
  dout(30) << __func__ << " thinstore == " <<  oid
	   << " 0x" << std::hex << offset << "~" << length << std::dec
	   << " sd_index " << sd_index << dendl;
  ThOnodeRef o = get_thonode(oid, false, sd_index);
  if (!o || !(o->thonode.block_maps)) {
    dout(0) << __func__ << " invalid or no blocks " << dendl;
    return -ENOENT;
  }
  if (!o->thonode.size) {
    return -ENOENT;
  }
  // ToDo: Multiple blocks read
  int n_head = 0, n_body = 0, n_tail = 0;
  get_head_body_tail(n_head, n_body, n_tail, offset, length, block_size);
  bool get_addr_result;
  uint32_t cur_offset = offset;
  uint64_t next_addr = 0;
  uint64_t cur_addr = o->thonode.block_maps->get_addr_by_offset(cur_offset, get_addr_result);
  int read_len = 0;
  bool need_read = true;
  bufferlist buffer;
  uint32_t remain = offset % block_size;
  int r;
  uint64_t start_addr = cur_addr;
  if (remain) {
    if (offset < block_size) {
      cur_offset = offset - remain;
    } else {
      cur_offset = offset - (block_size - remain);
    }
  }

  // read until continous offset
  while (cur_offset < offset + length) {
    next_addr = 
	o->thonode.block_maps->get_addr_by_offset(cur_offset + block_size,
							  get_addr_result);
    need_read = true;
    read_len += block_size;
    dout(40) << __func__ << " offset " << offset << " cur_offset " 
	    << cur_offset << " cur_addr " << cur_addr << " next_addr " 
	    << next_addr << " read_len " << read_len 
	    << " buffer length " << buffer.length() 
	    << " oid " << o->thonode.oid 
	    << " block_maps block size " << o->thonode.block_size 
	    << dendl;
    if (next_addr != cur_addr + block_size) {
      bufferlist tmp_buf;
      if (start_addr < part_desc[sd_index].d_start ||
	  start_addr > (part_desc[sd_index].d_start + part_desc[sd_index].d_len)) {
	dout(0) << __func__ << " thonode: " << o->thonode << dendl;
	ceph_assert(0);
      }
      if (!get_addr_result || next_addr == 0) {
	break;
      }
      r = bdev->read(start_addr, 
			read_len, &tmp_buf, NULL,
			false);
      if (r < 0) { 
	dout(0) << __func__ << " error " << r << dendl;
	ceph_assert(0);
      }
      start_addr = cur_addr;
      buffer.append(tmp_buf);
      need_read = false;
      //read_len -= tmp_buf.length();
      read_len -= tmp_buf.length();
      ceph_assert(read_len >= 0);
    } else {
      if (!get_addr_result || next_addr == 0) {
	break;
      }
      //read_len += block_size;
    }

    cur_addr = next_addr;
    cur_offset += block_size;
    if (o->thonode.size <= cur_offset) {
      break;
    }
  }

  if (start_addr < part_desc[sd_index].d_start ||
      start_addr > (part_desc[sd_index].d_start + part_desc[sd_index].d_len)) {
    ceph_assert(0);
  }

  // read remain data
  if (need_read) {
    bufferlist tmp_buf;
    dout(40) << __func__ << " cur_addr " << cur_addr << " read_len " 
	    << read_len << dendl; 
    r = bdev->read(start_addr, 
		      read_len, &tmp_buf, NULL,
		      false);
    if (r < 0) { 
      dout(0) << __func__ << " error " << r << dendl;
      ceph_assert(0);
    }
    buffer.append(tmp_buf);
  }

  uint64_t real_length = length;
  if (offset + length > o->thonode.size) {
    if (o->thonode.size <= offset) {
      real_length = o->thonode.size;
      dout(0) << __func__ << " warning thonode.size <= offset " << dendl;
    } else {
      real_length = o->thonode.size - offset;
    }
  }

  ceph_assert(real_length < OBJECT_MAX_SIZE);
  if (buffer.length() != real_length) {
    dout(0) << __func__ << " omw warning !!" << dendl;
    dout(0) << __func__ << " remain " << remain << " try to read length " << length
	    << " buffer length " << buffer.length() << " bl length " << bl.length() 
	    << " real length " << real_length << " obj size " << o->thonode.size
	    << dendl; 
  }

  bufferptr bptr(real_length);
  bl.push_back(std::move(bptr));
  bl.copy_in(0, real_length, buffer.c_str() + remain);

  dout(40) << __func__ << " remain " << remain << " try to read length " << length
	  << " buffer length " << buffer.length() << " bl length " << bl.length() 
	  << " real length " << real_length << " obj size " << o->thonode.size
	  << dendl; 
  return 0;
}

#if 0
  dout(40) << __func__ << " " <<  oid
	   << " 0x" << std::hex << offset << "~" << length << std::dec
	   << " = " << r << dendl;
  uint64_t raw_start_offset = part_desc[sd_index].d_start + 
		      (o->thonode.block_maps->block_maps[0] * block_size) +
		      offset;
  int r = _do_read(o, raw_start_offset, length, bl, op_flags);
  if (r < 0) {
    dout(0) << __func__ << " error " << r << dendl;
    assert(0);
  }
#endif

int ThinStore::_do_read(
  ThOnodeRef o,
  uint64_t offset,
  size_t length,
  bufferlist& bl,
  uint32_t op_flags,
  uint64_t retry_count,
  int sd_index)
{
  dout(0) << __func__ << " 0x" << std::hex << offset << "~" << length
           << " size 0x" << o->thonode.size << " (" << std::dec
           << o->thonode.size << ")" << dendl;
  bl.clear();

  if (offset >= o->thonode.size) {
    return -EIO;
  }

  if (offset + length > o->thonode.size) {
    length = o->thonode.size - offset;
  }

  ThIOContext thioc(cct, NULL, true); // allow EIO
  int r = bdev->aio_read(offset, length, &bl, &thioc);
  //r = bdev->read(offset, length, &req.bl, &ioc, false);
  int64_t num_ios;
  if (thioc.has_pending_aios()) {
    num_ios =- thioc.get_num_ios();
    bdev->aio_submit(&thioc);
    dout(0) << __func__ << " waiting for aio" << dendl;
    thioc.aio_wait();
    r = thioc.get_return_value();
    if (r < 0) {
      ceph_assert(r == -EIO); // no other errors allowed
      return -EIO;
    }
  }
  dout(0) << __func__ << " result " << bl.length() << dendl;
  return r;
}


int ThinStore::mkfs()
{
  dout(1) << __func__ << " path " << path << dendl;
  int r;
  int number_of_partitions = cct->_conf->osd_threads_sd;
  num_partitions = number_of_partitions;
  part_desc = new ThPartitionDesc[number_of_partitions];
  // must be 2 4 8 16...
  //uint64_t thin_store_size = bdev->get_size() / 8;
  uint64_t thin_store_size = bdev->get_size();
  thin_store_size = align_byte(thin_store_size);
  uint64_t start_point = 0;
  //uint64_t start_point = thin_store_size;
  //start_point = align_byte(start_point);
  uint64_t partition_size = thin_store_size / number_of_partitions;
  partition_size = align_byte(partition_size);
  partition_journal_size = partition_size / 10;
  partition_journal_size = align_byte(partition_journal_size);
  partition_meta_size = partition_size / 10;
  partition_meta_size = align_byte(partition_meta_size);
  partition_alloc_size = partition_size / 10;
  partition_alloc_size = align_byte(partition_alloc_size);
  dout(0) << __func__ << " partition size " << partition_size
	  << " partition journal size " << partition_journal_size << dendl;



  // ToDo: make super block

  // always initailize partition table
  for (int i = 0; i < number_of_partitions; i++) {
    // ToDo: fixed boundary
    // journal: 2GB meta: 16GB Alloc: 2GB Data: the rest of disk
    part_desc[i].block_size = bdev->block_size;
    part_desc[i].j_start = start_point + partition_size * i;
    part_desc[i].j_len = partition_journal_size;
    part_desc[i].m_start = part_desc[i].j_start + part_desc[i].j_len;
    part_desc[i].m_len = partition_meta_size;
    part_desc[i].a_start = part_desc[i].m_start + part_desc[i].m_len;
    part_desc[i].a_len = partition_alloc_size;
    part_desc[i].d_start = part_desc[i].a_start + part_desc[i].a_len;
    part_desc[i].d_len = partition_size - partition_journal_size 
			- partition_meta_size - partition_alloc_size;
    dout(0) << __func__ << " index " << i << " journal start " << part_desc[i].j_start 
	    << " journal len " << part_desc[i].j_len << " meta start "
	    << part_desc[i].m_start << " meta len " << part_desc[i].m_len
	    << " alloc start " << part_desc[i].a_start << " alloc len "
	    << part_desc[i].a_len << " data start " << part_desc[i].d_start
	    << " data len " << part_desc[i].d_len << dendl;
    // journal
    part_desc[i].j_desc.block_size = bdev->block_size;
    part_desc[i].j_desc.size = partition_journal_size;
    part_desc[i].j_desc.cur_seq = 0;
    part_desc[i].j_desc.cur_off = 0;
    struct ThJournalEntry j_entry;
    part_desc[i].j_desc.entry_size = j_entry.get_size();
    part_desc[i].j_desc.max_entry_size = 
		part_desc[i].j_desc.size / 
		part_desc[i].j_desc.entry_size;
    part_desc[i].j_desc.type= TH_JOURNAL;

    // meta
    part_desc[i].m_desc.block_size = bdev->block_size;
    part_desc[i].m_desc.size = partition_meta_size;
    part_desc[i].m_desc.cur_seq = 0;
    part_desc[i].m_desc.cur_off = 0;
    struct ThMetaEntry m_entry;
    part_desc[i].m_desc.entry_size = m_entry.get_size();
    part_desc[i].m_desc.max_entry_size = 
		part_desc[i].m_desc.size /
		part_desc[i].m_desc.entry_size;
    part_desc[i].m_desc.type= TH_META;

    dout(0) << __func__ << " meta max entry size " 
	    << part_desc[i].m_desc.max_entry_size 
	    << " meta entry size " << part_desc[i].m_desc.entry_size << dendl;


    bufferlist bl;
    bufferptr null_data;
    uint64_t pos = part_desc[i].m_start;
    uint32_t len = 1024 * 1024;
    bl.append_zero(len);
    for (; pos < part_desc[i].m_start + part_desc[i].m_len; 
	  pos += len) {
      int r = bdev->write(pos, bl, false, 0);
      if (r < 0) {
	dout(0) << __func__ << " error: " << r << " pos " << pos << " length " << bl.length() << dendl;
	assert(0);
      }
      ceph_assert(bl.length());
    }
    dout(0) << __func__ << " pos " << pos << " length " << bl.length() << dendl;


    // alloc 
    part_desc[i].a_desc.block_size = bdev->block_size;
    part_desc[i].a_desc.size = partition_alloc_size;
    part_desc[i].a_desc.cur_seq = 0;
    part_desc[i].a_desc.cur_off = 0;
    struct ThAllocEntry a_entry;
    part_desc[i].a_desc.entry_size = a_entry.get_size();
    part_desc[i].a_desc.max_entry_size = 
		part_desc[i].a_desc.size / 
		part_desc[i].a_desc.entry_size;
    part_desc[i].a_desc.type= TH_ALLOC;

    nid_last.push_back(0);

    alloc.push_back(new ThAllocator(cct, &part_desc[i].a_desc, i, this));
    alloc[i]->init();
    //ceph_assert(alloc[i]->cache_entry_size * sizeof(uint32_t) < block_size);


    pos = part_desc[i].a_start;
    // initialize alloc area to preseve persistent state
    for (; pos < part_desc[i].a_start + part_desc[i].a_len; 
	  pos += len) {
      int r = bdev->write(pos, bl, false, 0);
      if (r < 0) {
	dout(0) << __func__ << " error: " << r << " pos " << pos << " length " << bl.length() << dendl;
	assert(0);
      }
      ceph_assert(bl.length());
    }
    dout(0) << __func__ << " pos " << pos << " length " << bl.length() << dendl;

    // initialize onode cache
    thonode_map.push_back( new unordered_map<hobject_t,ThOnodeRef>() );
    //make_pair(ghobject_t(), NULL) );
    part_desc[i].d_desc.journal_pointer = part_desc[i].d_start;
    part_desc[i].d_desc.block_size = bdev->block_size;
    part_desc[i].d_desc.max_entry_size = 
		part_desc[i].d_len / 
		part_desc[i].d_desc.block_size;
    dout(0) << __func__ << " thonode_map size " << thonode_map.size() << dendl;
  }

  ceph_assert(thonode_map.size());
  dout(0) << __func__ << " thonode_map size " << thonode_map.size() << dendl;
  dout(1) << __func__ << " initialization is done " << path << dendl;
}

int ThinStore::umount() {
  dout(0) << __func__ << " thinstore " << dendl;
  for (auto p : thonode_map) {
    delete p;
  }
}

#if 0
    // data
    part_desc[i].j_desc.block_size = bdev->block_size;
    part_desc[i].j_desc.size = part_desc[i].j_len;
    part_desc[i].j_desc.cur_seq = 0;
    part_desc[i].j_desc.cur_off = 0;
    part_desc[i].j_desc.entry_size = sizeof(struct ThDataEntry);
    part_desc[i].j_desc.type= TH_DATA;
#endif

#if 0
  uuid_d old_fsid;

  {
    string done;
    r = read_meta("mkfs_done", &done);
    if (r == 0) {
      dout(1) << __func__ << " already created" << dendl;
      return r; // idempotent
    }
  }

  {
    string type;
    r = read_meta("type", &type);
    if (r == 0) {
      if (type != "thinstore") {
	derr << __func__ << " expected bluestore, but type is " << type << dendl;
	return -EIO;
      }
    } else {
      r = write_meta("type", "thinstore");
      if (r < 0)
        return r;
    }
  }

  freelist_type = "bitmap";

  r = _open_path();
  if (r < 0)
    return r;

  r = _open_fsid(true);
  if (r < 0)
    goto out_path_fd;

  r = _lock_fsid();
  if (r < 0)
    goto out_close_fsid;

  r = _read_fsid(&old_fsid);
  if (r < 0 || old_fsid.is_zero()) {
    if (fsid.is_zero()) {
      fsid.generate_random();
      dout(1) << __func__ << " generated fsid " << fsid << dendl;
    } else {
      dout(1) << __func__ << " using provided fsid " << fsid << dendl;
    }
    // we'll write it later.
  } else {
    if (!fsid.is_zero() && fsid != old_fsid) {
      derr << __func__ << " on-disk fsid " << old_fsid
	   << " != provided " << fsid << dendl;
      r = -EINVAL;
      goto out_close_fsid;
    }
    fsid = old_fsid;
  }

  r = _setup_block_symlink_or_file("block", cct->_conf->bluestore_block_path,
				   cct->_conf->bluestore_block_size,
				   cct->_conf->bluestore_block_create);
  if (r < 0)
    goto out_close_fsid;
  if (cct->_conf->bluestore_bluefs) {
    r = _setup_block_symlink_or_file("block.wal", cct->_conf->bluestore_block_wal_path,
	cct->_conf->bluestore_block_wal_size,
	cct->_conf->bluestore_block_wal_create);
    if (r < 0)
      goto out_close_fsid;
    r = _setup_block_symlink_or_file("block.db", cct->_conf->bluestore_block_db_path,
	cct->_conf->bluestore_block_db_size,
	cct->_conf->bluestore_block_db_create);
    if (r < 0)
      goto out_close_fsid;
  }

  r = _open_bdev(true);
  if (r < 0)
    goto out_close_fsid;

  // choose min_alloc_size
  if (cct->_conf->bluestore_min_alloc_size) {
    min_alloc_size = cct->_conf->bluestore_min_alloc_size;
  } else {
    ceph_assert(bdev);
    if (bdev->is_rotational()) {
      min_alloc_size = cct->_conf->bluestore_min_alloc_size_hdd;
    } else {
      min_alloc_size = cct->_conf->bluestore_min_alloc_size_ssd;
    }
  }
  _validate_bdev();

  // make sure min_alloc_size is power of 2 aligned.
  if (!isp2(min_alloc_size)) {
    derr << __func__ << " min_alloc_size 0x"
	 << std::hex << min_alloc_size << std::dec
	 << " is not power of 2 aligned!"
	 << dendl;
    r = -EINVAL;
    goto out_close_bdev;
  }

  r = _open_db(true);
  if (r < 0)
    goto out_close_bdev;

  {
    KeyValueDB::Transaction t = db->get_transaction();
    r = _open_fm(t);
    if (r < 0)
      goto out_close_db;
    {
      bufferlist bl;
      encode((uint64_t)0, bl);
      t->set(PREFIX_SUPER, "nid_max", bl);
      t->set(PREFIX_SUPER, "blobid_max", bl);
    }

    {
      bufferlist bl;
      encode((uint64_t)min_alloc_size, bl);
      t->set(PREFIX_SUPER, "min_alloc_size", bl);
    }

    ondisk_format = latest_ondisk_format;
    _prepare_ondisk_format_super(t);
    db->submit_transaction_sync(t);
  }

  r = write_meta("kv_backend", cct->_conf->bluestore_kvbackend);
  if (r < 0)
    goto out_close_fm;

  r = write_meta("bluefs", stringify(bluefs ? 1 : 0));
  if (r < 0)
    goto out_close_fm;

  if (fsid != old_fsid) {
    r = _write_fsid();
    if (r < 0) {
      derr << __func__ << " error writing fsid: " << cpp_strerror(r) << dendl;
      goto out_close_fm;
    }
  }

  if (out_of_sync_fm.fetch_and(0)) {
    _sync_bluefs_and_fm();
  }

 out_close_fm:
  _close_fm();
 out_close_db:
  _close_db();
 out_close_bdev:
  _close_bdev();
 out_close_fsid:
  _close_fsid();
 out_path_fd:
  _close_path();

  if (r == 0 &&
      cct->_conf->bluestore_fsck_on_mkfs) {
    int rc = fsck(cct->_conf->bluestore_fsck_on_mkfs_deep);
    if (rc < 0)
      return rc;
    if (rc > 0) {
      derr << __func__ << " fsck found " << rc << " errors" << dendl;
      r = -EIO;
    }
  }

  if (r == 0) {
    // indicate success by writing the 'mkfs_done' file
    r = write_meta("mkfs_done", "yes");
  }

  if (r < 0) {
    derr << __func__ << " failed, " << cpp_strerror(r) << dendl;
  } else {
    dout(0) << __func__ << " success" << dendl;
  }
  return r;
#endif

#if 0
int ThinStore::_read_bdev_label(CephContext* cct, string path,
				thinstore_bdev_label_t *label)
{
  dout(10) << __func__ << dendl;
  int fd = TEMP_FAILURE_RETRY(::open(path.c_str(), O_RDONLY|O_CLOEXEC));
  if (fd < 0) {
    fd = -errno;
    derr << __func__ << " failed to open " << path << ": " << cpp_strerror(fd)
	 << dendl;
    return fd;
  }
  bufferlist bl;
  int r = bl.read_fd(fd, BDEV_LABEL_BLOCK_SIZE);
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  if (r < 0) {
    derr << __func__ << " failed to read from " << path
	 << ": " << cpp_strerror(r) << dendl;
    return r;
  }

  uint32_t crc, expected_crc;
  auto p = bl.cbegin();
  try {
    decode(*label, p);
    bufferlist t;
    t.substr_of(bl, 0, p.get_off());
    crc = t.crc32c(-1);
    decode(expected_crc, p);
  }
  catch (buffer::error& e) {
    dout(2) << __func__ << " unable to decode label at offset " << p.get_off()
	 << ": " << e.what()
	 << dendl;
    return -ENOENT;
  }
  if (crc != expected_crc) {
    derr << __func__ << " bad crc on label, expected " << expected_crc
	 << " != actual " << crc << dendl;
    return -EIO;
  }
  dout(10) << __func__ << " got " << *label << dendl;
  return 0;
}

int ThinStore::_write_bdev_label(CephContext *cct,
				 string path, bluestore_bdev_label_t label)
{
  dout(10) << __func__ << " path " << path << " label " << label << dendl;
  bufferlist bl;
  encode(label, bl);
  uint32_t crc = bl.crc32c(-1);
  encode(crc, bl);
  ceph_assert(bl.length() <= BDEV_LABEL_BLOCK_SIZE);
  bufferptr z(BDEV_LABEL_BLOCK_SIZE - bl.length());
  z.zero();
  bl.append(std::move(z));

  int fd = TEMP_FAILURE_RETRY(::open(path.c_str(), O_WRONLY|O_CLOEXEC));
  if (fd < 0) {
    fd = -errno;
    derr << __func__ << " failed to open " << path << ": " << cpp_strerror(fd)
	 << dendl;
    return fd;
  }
  int r = bl.write_fd(fd);
  if (r < 0) {
    derr << __func__ << " failed to write to " << path
	 << ": " << cpp_strerror(r) << dendl;
    goto out;
  }
  r = ::fsync(fd);
  if (r < 0) {
    derr << __func__ << " failed to fsync " << path
	 << ": " << cpp_strerror(r) << dendl;
  }
out:
  VOID_TEMP_FAILURE_RETRY(::close(fd));
  return r;
}

int ThinStore::read_meta(const std::string& key, std::string *value)
{
  thinstore_bdev_label_t label;
  string p = path + "/block";
  int r = _read_bdev_label(cct, p, &label);
  if (r < 0) {
    return ObjectStore::read_meta(key, value);
  }
  auto i = label.meta.find(key);
  if (i == label.meta.end()) {
    return ObjectStore::read_meta(key, value);
  }
  *value = i->second;
  return 0;
}

int ThinStore::write_meta(const std::string& key, const std::string& value)
{
  bluestore_bdev_label_t label;
  string p = path + "/block";
  int r = _read_bdev_label(cct, p, &label);
  if (r < 0) {
    return ObjectStore::write_meta(key, value);
  }
  label.meta[key] = value;
  r = _write_bdev_label(cct, p, label);
  ceph_assert(r == 0);
  return ObjectStore::write_meta(key, value);
}
#endif

template<typename S>
static string pretty_binary_string(const S& in)
{
  char buf[10];
  string out;
  out.reserve(in.length() * 3);
  enum { NONE, HEX, STRING } mode = NONE;
  unsigned from = 0, i;
  for (i=0; i < in.length(); ++i) {
    if ((in[i] < 32 || (unsigned char)in[i] > 126) ||
	(mode == HEX && in.length() - i >= 4 &&
	 ((in[i] < 32 || (unsigned char)in[i] > 126) ||
	  (in[i+1] < 32 || (unsigned char)in[i+1] > 126) ||
	  (in[i+2] < 32 || (unsigned char)in[i+2] > 126) ||
	  (in[i+3] < 32 || (unsigned char)in[i+3] > 126)))) {
      if (mode == STRING) {
	out.append(in.c_str() + from, i - from);
	out.push_back('\'');
      }
      if (mode != HEX) {
	out.append("0x");
	mode = HEX;
      }
      if (in.length() - i >= 4) {
	// print a whole u32 at once
	snprintf(buf, sizeof(buf), "%08x",
		 (uint32_t)(((unsigned char)in[i] << 24) |
			    ((unsigned char)in[i+1] << 16) |
			    ((unsigned char)in[i+2] << 8) |
			    ((unsigned char)in[i+3] << 0)));
	i += 3;
      } else {
	snprintf(buf, sizeof(buf), "%02x", (int)(unsigned char)in[i]);
      }
      out.append(buf);
    } else {
      if (mode != STRING) {
	out.push_back('\'');
	mode = STRING;
	from = i;
      }
    }
  }
  if (mode == STRING) {
    out.append(in.c_str() + from, i - from);
    out.push_back('\'');
  }
  return out;
}

#if 0
  uint32_t cursor = 0;
  bool check_start = false;
    if (o->thonode.block_maps->block_maps[i] == 0 && check_start) {
      //cursor++;
      vector<uint32_t> block_no = (alloc[sd_index])->alloc_data_block(1, sd_index);
      o->thonode.block_maps->block_maps[i] = block_no[0];

    } else if (o->thonode.block_maps->block_maps[i] == 0 && !check_start) {
      check_start = true;
      //cursor++;
    } else {
      check_start = false;
      if (cursor) {
	vector<uint32_t> block_no = (alloc[sd_index])->alloc_data_block(cursor, sd_index);
	
      }
    }
#endif


#if 0
    _do_raw_write(d_start_offset + (offset % block_size), block_size - (offset % block_size),
		  0, bl, sd_index);
    d_start_offset = part_desc[sd_index]->d_start + (block_size * (*iter));
    _do_raw_write(d_start_offset, w_length,
		  pos_offset, bl, sd_index);
#endif

// backup code
#if 0
  uint32_t need_blocks = total_block_len + block_start - i;
  vector<uint32_t> block_no = (alloc[sd_index])->alloc_data_block(need_blocks, sd_index);

  // ToDo: alloc blocks (make continus block)
  //thioc->allc_data_block_no = alloc->alloc_data_block(total_block_len);
  ceph_assert(alloc[sd_index]);
  vector<uint32_t> block_no = (alloc[sd_index])->alloc_data_block(total_block_len, sd_index);
  ceph_assert(block_no.size());
  
  //assert(bl.length() <= n_head + n_body + n_tail);

  // find location
  vector<uint32_t>::iterator iter = block_no.begin();
  uint64_t d_start_offset;
  uint64_t pos_offset = 0;
  if (n_head) {
    d_start_offset = part_desc[sd_index].d_start + (block_size * block_no[0]) + (offset % block_size);
    iter++;
    pos_offset += block_size - (offset % block_size);
  } else {
    d_start_offset = part_desc[sd_index].d_start + (block_size * block_no[0]);
  }

  for (;iter != block_no.end(); ++iter) {
    uint64_t w_length = block_size;
    if (pos_offset + block_size > bl.length()) {
      w_length = bl.length() - pos_offset;
    }
    pos_offset += w_length;
  }

#endif

