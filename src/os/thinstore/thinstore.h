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

#ifndef CEPH_OSD_THINSTORE_H
#define CEPH_OSD_THINSTORE_H

#include "acconfig.h"

#include <unistd.h>

#include <atomic>
#include <mutex>
#include <condition_variable>

#include <boost/intrusive/list.hpp>
#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive/set.hpp>
#include <boost/functional/hash.hpp>
#include <boost/dynamic_bitset.hpp>

#include "include/ceph_assert.h"
#include "include/unordered_map.h"
#include "include/mempool.h"

//#include "include/encoding.h"

#include "common/bloom_filter.hpp"
#include "common/Finisher.h"
#include "common/Throttle.h"
#include "common/perf_counters.h"
#include "common/PriorityCache.h"
#include "compressor/Compressor.h"
#include "os/ObjectStore.h"

#include "thinstore_types.h"
#include "os/bluestore/bluestore_types.h"
#include "os/bluestore/BlueStore.h"
#include "ThBlockDevice.h"
#include "ThKernelDevice.h"
#include "common/EventTrace.h"

#if 0
using ceph::decode;
using ceph::decode_nohead;
using ceph::encode;
using ceph::encode_nohead;
#endif


/// an in-memory object
struct ThOnode {
  //MEMPOOL_CLASS_HELPERS();

  //Collection *c;

  ghobject_t oid;

  /// key under PREFIX_OBJ where we are stored
  mempool::bluestore_cache_other::string key;

  bool exists;              ///< true if object logically exists

  ThMetaEntry thonode;

  ceph::mutex flush_lock = ceph::make_mutex("ThStore::ThOnode::flush_lock");
  ceph::condition_variable flush_cond;   ///< wait here for uncommitted txns
  int nref;

#if 0
  ThOnode(Collection *c, const ghobject_t& o,
	const mempool::bluestore_cache_other::string& k)
#endif
  ThOnode(const ghobject_t& o,
	const mempool::bluestore_cache_other::string& k)
    : 
      oid(o),
      key(k),
      exists(false) {
    nref = 0;
  }
  ThOnode(const ghobject_t& o)
    : 
      oid(o),
      exists(false) {
    nref = 0;
  }
  ThOnode()
    : 
      exists(false) {
    nref = 0;
  }

  //void dump(Formatter* f) const;

  void flush() {}
  void get() {
    ++nref;
  }
  void put() {
    if (--nref == 0) 
      delete this;
  }
};

typedef boost::intrusive_ptr<ThOnode> ThOnodeRef;
#if 0
MEMPOOL_DEFINE_OBJECT_FACTORY(ThOnode, thinstore_thonode,
                              bluestore_cache_onode);
#endif

struct ThTransContext;
class ThAllocator;

class ThinStore : public ObjectStore {
		  //public md_config_obs_t {
  struct ThPartitionDesc * part_desc;
  BlueStore * bluestore;
  ThBlockDevice * bdev;
  vector<ThAllocator *> alloc;
  vector<uint64_t> nid_last;
  uint64_t block_size = 0;     ///< block size of block device (power of 2)
  uint64_t block_mask = 0;     ///< mask to get just the block offset
  size_t block_size_order = 0; ///< bits to shift to get block size
  uint64_t partition_journal_size;
  uint64_t partition_meta_size;
  uint64_t partition_alloc_size;
  bool block_pre_allocation = false;
  bool block_pre_allocation_4mb = true;
  bool enable_async_write = false;
  bool journal_mode = false;
  vector< unordered_map<hobject_t,ThOnodeRef> *> thonode_map;
  uint64_t g_seg_id = 0;
  int whoami = -1;



  struct Collection : public CollectionImpl {
    ThinStore *store;
    Collection(ThinStore *ns, coll_t c);
    ContextQueue *commit_queue;
  };
  typedef boost::intrusive_ptr<Collection> CollectionRef;
  int num_partitions;
  int path_fd = -1;
  int _open_path();
  void _close_path();

  void set_orig_store(ObjectStore *ostore, int whoami) {
    bluestore = static_cast<BlueStore*>(ostore);
    this->whoami = whoami;
  }
  int _open_bdev(bool);
  int mount();
  void _assign_nid(ThTransContext *txc, ThOnodeRef& o, int sd_index);
  int mkfs();
  int queue_transactions(
	      CollectionHandle& ch,
	      vector<Transaction>& tls,
	      TrackedOpRef op,
	      ThreadPool::TPHandle *handle) { return -EOPNOTSUPP; }
  int queue_transactions(
	      CollectionHandle& ch,
	      vector<Transaction>& tls,
	      TrackedOpRef op,
	      ThreadPool::TPHandle *handle,
	      int sd_index);
  int queue_transactions(
	      CollectionHandle& ch,
	      vector<struct sd_entry*> ops_list,
	      int sd_index);
  int queue_transactions_in_place(
	      CollectionHandle& ch,
	      vector<struct sd_entry *> ops_list,
	      int sd_index,
	      bool journal_mode);
  void _txc_add_transaction(ThTransContext *txc, Transaction *t, int sd_index, ThIOContext *thioc);
  int _write(ThTransContext *txc,
			ThOnodeRef& o,
			uint64_t offset, size_t length,
			bufferlist& bl,
			uint32_t fadvise_flags,
			ThIOContext *thioc,
			int sd_index);
  int _do_write(ThOnodeRef& o, uint64_t offset, size_t length, bufferlist& bl, int sd_index, 
		  ThIOContext *thioc);
  void _do_raw_write(uint64_t offset, uint64_t length, uint64_t bl_offset,
		      bufferlist& bl, uint32_t sd_index, ThIOContext *thioc);
  int _do_meta_write(ThOnodeRef& o, int sd_index, ThIOContext *thioc);
  uint32_t get_thmeta_location(ThOnodeRef& o, int sd_index);
  int read(CollectionHandle &c_, const ghobject_t& oid, uint64_t offset,
	  size_t length, bufferlist& bl, uint32_t op_flags) { return -EOPNOTSUPP; }
  int read(CollectionHandle &c_, const ghobject_t& oid, uint64_t offset,
	  size_t length, bufferlist& bl, uint32_t op_flags, int sd_index);
  int _do_read(ThOnodeRef o, uint64_t offset, size_t length,
	      bufferlist& bl, uint32_t op_flags, uint64_t retry_count = 0, 
	      int sd_index = 0);
  int _do_journal_write(uint32_t op,
      ThOnodeRef& o,
      uint64_t offset, size_t length,
      bufferlist& bl,
      uint32_t fadvise_flags,
      int sd_index,
      ThIOContext *thioc,
      vector<uint32_t> *alloc_data_block_no);

  void read_thonode(ThOnodeRef o, const ghobject_t& oid, int sd_index);
  ThOnodeRef get_thonode(
    const ghobject_t& oid,
    bool create,
    int sd_index);
  bool is_new_thonode(ThOnodeRef o);
  void write_thonode(ThOnodeRef o, int sd_index);
  void _journal_write(ThOnodeRef o, ThIOContext * thioc, bufferlist &bl, ThTransContext * txc);
  int do_journal_write(int sd_index, ThIOContext * thioc, ThTransContext & txc);


#if 0
  virtual const char** get_tracked_conf_keys() const { return NULL; }
  virtual void handle_conf_change(const ConfigProxy& conf,
#endif
  objectstore_perf_stat_t get_cur_stats() {return objectstore_perf_stat_t();}
  const PerfCounters* get_perf_counters() const {return NULL;}
  std::string get_type() { return std::string(); }
  bool test_mount_in_use() { return true; }
  int umount();
  int validate_hobject_key(const hobject_t &obj) const { return 0; }
  unsigned get_max_attr_name_length() { return 0;}
  int mkjournal() { return 1; } // journal only
  bool needs_journal() { return true; } //< requires a journal
  bool wants_journal() { return true; } //< prefers a journal
  bool allows_journal() { return true; } //< allows a journal
  int statfs(struct store_statfs_t *buf, osd_alert_list_t* alerts) { return 1; }
  int pool_statfs(uint64_t pool_id, struct store_statfs_t *buf) { return 1; }
  CollectionHandle open_collection(const coll_t &cid) { return CollectionRef(); }
  CollectionHandle create_new_collection(const coll_t &cid) { return CollectionRef(); }
  void set_collection_commit_queue(const coll_t &cid, ContextQueue *commit_queue) {}
  bool exists(CollectionHandle& c, const ghobject_t& oid) { return true; }
  int set_collection_opts(CollectionHandle& ch, const pool_opts_t& opts) { return 1; }
  int stat(CollectionHandle &c_, const ghobject_t& oid, struct stat *st, bool allow_eio) { return 1; }
  int fiemap(CollectionHandle& c, const ghobject_t& oid, 
              uint64_t offset, size_t length, bufferlist& bl) { return 1; }
  int fiemap(CollectionHandle& c, const ghobject_t& oid,
      uint64_t offset, size_t len, std::map<uint64_t, uint64_t>& destmap) { return 1; }
  int getattr(CollectionHandle &c, const ghobject_t& oid,
                        const char *name, ceph::buffer::ptr& value) { return 1; }
  int getattr(CollectionHandle &c, const ghobject_t& oid, 
              const std::string& name, ceph::buffer::list& value) { return 1; }
  int getattrs(CollectionHandle &c, const ghobject_t& oid,
                         std::map<std::string,ceph::buffer::ptr>& aset) { return 1; }
  int getattrs(CollectionHandle &c, const ghobject_t& oid,
              std::map<std::string,ceph::buffer::list>& aset) { return 1; }
  int list_collections(std::vector<coll_t>& ls) { return 1; }
  bool collection_exists(const coll_t& c) { return true; }
  int collection_empty(CollectionHandle& c, bool *empty) { return 1; }
  int collection_bits(CollectionHandle& c) { return 1; }
  int collection_list(CollectionHandle &c, const ghobject_t& start, 
        	      const ghobject_t& end, int max, vector<ghobject_t> *ls, 
        	      ghobject_t *pnext) { return 1; }
  int omap_get(
    CollectionHandle &c_,    ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    ) { return 1; }
  int omap_get_header(
    CollectionHandle &c_,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio ///< [in] don't assert on eio
    ) { return 1; }
  int omap_get_keys(
    CollectionHandle &c_,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    ) { return 1; }
  int omap_get_values(
    CollectionHandle &c_,        ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    ) { return 1; }
  int omap_check_keys(
    CollectionHandle &c_,    ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    ) { return 1; }
  ObjectMap::ObjectMapIterator get_omap_iterator(
    CollectionHandle &c_,              ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    ) { return ObjectMap::ObjectMapIterator(); }
  void set_fsid(uuid_d u) {}
  uuid_d get_fsid() { return uuid_d(); }
  uint64_t estimate_objects_overhead(uint64_t num_objects) { return 1; }

public:
  ThinStore(CephContext *cct, const string& path);
  ~ThinStore() override;
  friend class ThAllocator;
};

struct ThTransContext {
  uint64_t bytes;
  uint64_t last_nid;
  map<hobject_t, ThOnodeRef> cache_onode;
};
static inline void intrusive_ptr_add_ref(ThOnode *o) {
    o->get();
}
static inline void intrusive_ptr_release(ThOnode *o) {
    o->put();
}

#endif
