
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>

#include "include/types.h"
#include "include/compat.h"
#include "include/stringify.h"
#include "common/blkdev.h"
#include "common/errno.h"

#include "allocator.h"
#include "thinstore.h"
#include "common/debug.h"
#include "common/align.h"
#include "common/numa.h"


#include "global/global_context.h"

#define dout_context cct
#define dout_subsys ceph_subsys_bluestore

void ThAllocator::init() {
  // ToDo: need the change from volatile mode to non-volatile
  cache_last_alloc = 0;
  cache_last_alloc_start = true;
  // data allocation will be started at 1
  alloc_map_start_block_no = 0;
  memset(alloc_map, 0, sizeof(alloc_map));
  memset(alloc_map_used, 0, sizeof(alloc_map_used));
  cache_entry_size = PREPATCH_SIZE;
  free_block = cache_entry_size;
  // ToDo: initialize alloc_map
  for (int i = 0; i<PREPATCH_SIZE; i++) {
    alloc_map[i] = i+1;
  }

}

void ThAllocator::claim_more_block(int sd_index) {

  // ToDo: fragmentation
  // flush dirtied alloc
  ceph_assert(thinstore);
  bufferlist flush_bl;
  uint64_t start_offset = thinstore->part_desc[sd_index].a_start;
  if (alloc_map_start_block_no != 1) {
    start_offset +=
	(alloc_map_start_block_no * 
	thinstore->part_desc[sd_index].a_desc.entry_size);
  }

  // ToDo: make usable
  // need check
  // need check
  // need check
  // need check
  // need check
  // need check
  // need check
#if 0
  uint32_t * entries = new uint32_t[cache_entry_size];
  for (uint32_t i = 0; i < cache_entry_size; i++) {
    if (alloc_map_used[i]) {
      entries[i] = 1;
      encode(entries[i], flush_bl);
    } else {
      entries[i] = 0;
      encode(entries[i], flush_bl);
    }
  }
  dout(0) << __func__ << " start offset " << start_offset << " flush_bl length " 
	  << flush_bl.length() <<dendl;
  thinstore->bdev->write(start_offset, flush_bl, false, 0);
  for (uint32_t i = 0; i < cache_entry_size; i++) {
    if (alloc_map_used[i]) {
      entries[i] = 0;
    }
  }
  delete entries;
#endif
  // in-memory 
  memset(alloc_map_used, 0, sizeof(bool)*cache_entry_size);

  // find free blocks
  uint32_t new_alloc_map_start_block_no;
  if (alloc_map_start_block_no == 1) {
    new_alloc_map_start_block_no = alloc_map_start_block_no + cache_entry_size;
  } else {
    new_alloc_map_start_block_no = alloc_map_start_block_no + cache_entry_size;
  }

  if (new_alloc_map_start_block_no 
	>= thinstore->part_desc[sd_index].d_desc.max_entry_size) {
    dout(0) << __func__ << " full !!!!!!!!!!! " << dendl;
    dout(0) << __func__ << " full !!!!!!!!!!! " << dendl;
    dout(0) << __func__ << " full !!!!!!!!!!! " << dendl;
    dout(0) << __func__ << " full !!!!!!!!!!! " << dendl;
    dout(0) << __func__ << " full !!!!!!!!!!! " << dendl;
    dout(0) << __func__ << " full !!!!!!!!!!! " << dendl;
    ceph_assert(0);
    return;
  }

  start_offset = thinstore->part_desc[sd_index].a_start + 
		  (new_alloc_map_start_block_no * 
		  thinstore->part_desc[sd_index].a_desc.entry_size);
		  //+ (cache_entry_size * thinstore->block_size);

  dout(30) << __func__ << " alloc_map_start_block_no : "  << alloc_map_start_block_no
	  << " new_alloc_map_start_block_no " << new_alloc_map_start_block_no 
	  << " read start_offset " << start_offset 
	  << dendl;

  //read_free_entry
  bufferlist bl;
  if (new_alloc_map_start_block_no >= thinstore->part_desc[sd_index].a_desc.max_entry_size) {
    alloc_map_start_block_no = 0;
    new_alloc_map_start_block_no = alloc_map_start_block_no;
    dout(30) << __func__ << " warning entry size is over the limitation " << dendl;
  }

  // need check
  // need check
  // need check
  // need check
  // need check
  // need check
  // need check
#if 0
  thinstore->bdev->read(start_offset, 
		  PREPATCH_SIZE * thinstore->part_desc[sd_index].a_desc.entry_size,
		  &bl, NULL, false);
  ceph_assert(bl.length() > 0);

  struct ThAllocEntry * th_alloc_entry;
  th_alloc_entry = (struct ThAllocEntry*)bl.c_str();

  uint32_t allocated = 0;
  for (int i = 0; i < PREPATCH_SIZE; i++) {
    if ((*th_alloc_entry).allocated == 0) {
      alloc_map[i] = new_alloc_map_start_block_no + i;
      allocated++;
    }
    th_alloc_entry++;
  }
#endif

  // memory only
  uint32_t allocated = 0;
  for (int i = 0; i < PREPATCH_SIZE; i++) {
    alloc_map[i] = new_alloc_map_start_block_no + i;
    allocated++;
  }

  cache_entry_size = allocated;
  // one block is reserved
  free_block = allocated;
  alloc_map_start_block_no = new_alloc_map_start_block_no;
  cache_last_alloc = 0;
  cache_last_alloc_start = true;
  dout(0) << __func__ << " get free blocks and stored in cache : " 
	  << allocated << " renew alloc_map_start_block_no " 
	  << alloc_map_start_block_no << dendl;
}

vector<uint32_t> ThAllocator::alloc_data_block(uint32_t num_block, int sd_index) {
  // find empty blocks
  vector<uint32_t> free_blocks;
  uint32_t need_block = num_block;
  uint64_t last = 0;
  assert(need_block > 0);

#if 0
  dout(0) << __func__ << " start == " 
	  << " free block " << free_block
	  << " num block " << num_block << dendl;
#endif
  // if we allocate blocks, does allocator has enough space?
  if (free_block < num_block) {
    claim_more_block(sd_index);
  }

  ceph_assert(free_block >= num_block);

  if (cache_last_alloc_start) {
    last = 0;
    cache_last_alloc_start = false;
  } else {
    last = cache_last_alloc + 1;
  }

#if 0
  dout(0) << __func__ << " " << __LINE__ 
	  << " try cache_last_alloc index " 
	  << last << dendl;
#endif

  uint64_t cache_start_alloc = alloc_map[last];
  for (uint64_t i = last; i < cache_entry_size; i++) {
    // we skip block 0
    if (alloc_map[i] == 0) {
      free_block--;
      alloc_map_used[i] = true;
      cache_last_alloc = i;
      continue;
    }
    // need check
    // need check
    // need check
    // need check
    // need check
    // need check
    //free_blocks.push_back(alloc_map[i]);

    need_block--;
    free_block--;
    alloc_map_used[i] = true;
    cache_last_alloc = i;
    if (need_block <= 0) 
      break;
  }
  ceph_assert(need_block == 0);
  free_blocks.push_back(cache_start_alloc);
  free_blocks.push_back(num_block);

#if 0
  dout(0) << __func__ << " allocated block no: " << dendl;
  for (auto p : free_blocks) {
    dout(0) << p << " " << dendl;
  }
#endif
  total_block += free_blocks.size();
  dout(0) << " | cache_last_alloc " << cache_last_alloc 
	  << " allocated first block " << free_blocks.front()
	  << " total blocks " << total_block 
	  << " allocated block size " << free_blocks.size()
	  << " free block " << free_block
	  << " cache_last_alloc index " << cache_last_alloc
	  << " requested the number of block " << num_block
	  << dendl;

  return free_blocks;
}

void ThAllocator::free_data_block(vector<uint32_t> & block)
{ return; }
