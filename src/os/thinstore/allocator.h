#ifndef CEPH_OS_THINSTORE_ALLOCATOR_H
#define CEPH_OS_THINSTORE_ALLOCATOR_H

#include <mutex>
#include "include/btree_map.h"
#include "include/interval_set.h"
#include "common/ceph_mutex.h"
#include <vector>
#include "thinstore_types.h"


class ThinStore;
// need init!
class ThAllocator {
public:
  ThAllocator(CephContext *cct, ThAllocDesc * alloc, int sd_index, ThinStore * thin) : 
	      cct(cct), alloc_desc(alloc), sd_index(sd_index), thinstore(thin)
  { 
    cache_last_alloc = 0;
    alloc_map_start_block_no = 0;
    free_block = 0;
    total_block = 0;
  }
  CephContext *cct;
  vector<uint32_t> alloc_data_block(uint32_t num_block, int sd_index);
  void free_data_block(vector<uint32_t> & blocks);
  void init();
  void claim_more_block(int sd_index);
  ThAllocDesc * alloc_desc;
  ThinStore * thinstore;
  int sd_index;
  // cache
  uint32_t cache_last_alloc;
  uint32_t alloc_map_start_block_no;
  uint32_t alloc_map[PREPATCH_SIZE]; // block_no
  bool alloc_map_used[PREPATCH_SIZE];
  uint32_t cache_entry_size;
  uint64_t free_block;
  uint64_t total_block;
  bool cache_last_alloc_start;
};

#endif
