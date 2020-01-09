#ifndef CEPH_OSD_THINSTORE_BLUESTORE_TYPES_H
#define CEPH_OSD_THINSTORE_BLUESTORE_TYPES_H

#include <ostream>
#include <bitset>
#include <type_traits>
#include "include/types.h"
#include "include/interval_set.h"
#include "include/utime.h"
#include "common/hobject.h"
#include "common/Checksummer.h"
#include "include/mempool.h"

/// label for block device
struct thinstore_bdev_label_t {
  uuid_d osd_uuid;     ///< osd uuid
  uint64_t size;       ///< device size
  utime_t btime;       ///< birth time
  string description;  ///< device description

  map<string,string> meta; ///< {read,write}_meta() content from ObjectStore

#if 0
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
  void dump(Formatter *f) const;
  static void generate_test_instances(list<bluestore_bdev_label_t*>& o);
#endif
};
//WRITE_CLASS_ENCODER(thinstore_bdev_label_t)

#define SIZE_META_ENTRY 512
#define PER_ENTRY_IN_BLOCK 200
//#define BLOCK_SIZE (4096 * 16)
#define BLOCK_SIZE 4096
//#define MAX_OBJECT_SIZE (((uint64_t)1024*1024*1024) - (20*4096*16))
//#define MAX_OBJECT_SIZE (((uint64_t)32*1024*1024) - (20*4096*16))
#define MAX_OBJECT_SIZE (((uint64_t)16*1024*1024))
#define NUMBER_OF_BLOCK_MAPS ((MAX_OBJECT_SIZE) / BLOCK_SIZE)
#define ALIGN_BYTE 4096
#define DEFAULT_ALLOC_BLOCK ((1024*1024*4)/BLOCK_SIZE)
#define ALIGN_SIZE 512

// Related to Allocator
// prepatch alloc entry
#define PREPATCH_SIZE (1024*8)//10000

//////////////
/* 
  fixed offset
*/

struct ThOnode;
struct ThSuperBlock {
  uint32_t num_partition;
  uint64_t start_offset[20];
};

// overall lay-out
class DefaultDesc {
public:
  uint64_t block_size;
  uint64_t size; 
  uint64_t cur_seq;
  uint64_t cur_off; 
  uint64_t entry_size;
  uint64_t max_entry_size;
  uint64_t type;
};

class ThJournalDesc : public DefaultDesc {
  uint64_t test;
  uint32_t journal_entry_size;
} __attribute__ ((packed));

struct ThJournalEntry {
  object_t oid;
  uint64_t nid;
  uint32_t op;
  uint64_t offset;
  uint64_t length;
  // ToDo: spatial alloc?
  uint32_t alloc_no_begin;
  uint32_t alloc_no_end;
  uint32_t aligned_size;
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
  uint32_t get_size();
  ThJournalEntry(uint32_t entry_size):aligned_size(entry_size) {}
  ThJournalEntry() {}
};
WRITE_CLASS_ENCODER(ThJournalEntry)


class ThMetaDesc : public DefaultDesc {
  uint64_t test;
  uint32_t meta_entry_size;
} __attribute__ ((packed));

struct ThMetaBlockMap {
#if 0
  uint64_t block_offset[NUMBER_OF_BLOCK_MAPS];
  vector<uint64_t> block_offset(NUMBER_OF_BLOCK_MAPS, 0);
    block_offset.(NUMBER_OF_BLOCK_MAPS);
    memset(block_offset, 0, sizeof(block_offset));
    block_offset.reserve(NUMBER_OF_BLOCK_MAPS);
#endif
  map<uint32_t, uint64_t> block_offset;
  uint64_t block_size;
  ThMetaBlockMap(uint64_t block_size) : 
      block_size(block_size) {
    //block_offset.reserve(NUMBER_OF_BLOCK_MAPS);
  }
  void set_addr_by_offset(uint64_t offset, uint64_t addr) {
    uint32_t cur_offset = offset / block_size;
    block_offset[cur_offset] = addr;
  }
  uint64_t get_addr_by_offset(uint64_t offset, bool & result) {
    uint32_t cur_offset = offset / block_size;
    if (block_offset.empty()) {
      result = false;
      return 0;
    }
    if (block_offset.end() == block_offset.find(cur_offset)) {
      result = false;
      return 0;
    }
    result = true;
    return block_offset[cur_offset];
  }
  void set_addr_by_block_no(uint64_t block_no, uint64_t addr) {
    block_offset[block_no] = addr;
  }
  uint64_t get_addr_by_block_no(uint64_t block_no, bool & result) {
    if (block_offset.empty()) {
      result = false;
      return 0;
    }
    if (block_offset.end() == block_offset.find(block_no)) {
      result = false;
      return 0;
    }
    result = true;
    return block_offset[block_no];
  }
  uint64_t get_size() {
    using ::ceph::encode;
    map<uint32_t, uint64_t> test;
    bufferlist buf;
    test[0] = 0;
    encode(test, buf);
    return buf.length() * NUMBER_OF_BLOCK_MAPS;
  }
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
};
WRITE_CLASS_ENCODER(ThMetaBlockMap)
std::ostream& operator<<(std::ostream& out, const ThMetaBlockMap &map);

struct ThMetaEntry {
  uint64_t nid;
  uint64_t size = 0;
  uint64_t block_size = 0;
  bool has_block_map;
  hobject_t oid;
  ThMetaBlockMap * block_maps;
  // block map
  // ToDo: implement block map
  bool cached = false;
  bool need_flush_block_map = false;
  uint16_t encode_flag;
  uint32_t aligned_size;
  uint64_t mattrs_seg_id;
  ThMetaEntry(uint32_t align_size):block_maps(NULL), aligned_size(align_size) {
    get_size();
  }
  ThMetaEntry():block_maps(NULL) {
    get_size();
  }
  ~ThMetaEntry() {
    if (block_maps) 
      delete block_maps;
  }
  uint32_t get_size();
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
};
WRITE_CLASS_ENCODER(ThMetaEntry)
std::ostream& operator<<(std::ostream& out, const ThMetaEntry &en);


// == segment allocation info
class ThAllocDesc : public DefaultDesc {
  uint32_t test;
  uint32_t alloc_entry_size;
} __attribute__ ((packed));


// segmentation alloc ?
struct ThAllocEntry {
  uint32_t allocated = 0;
  uint32_t get_size() {
    return sizeof(uint32_t);
  }
} __attribute__ ((packed));

class ThDataDesc : public DefaultDesc {
public:
  uint64_t journal_pointer;
} __attribute__ ((packed));

struct ThPartitionDesc {
  uint64_t block_size;
  uint64_t j_start; // journal
  uint64_t j_len;
  uint64_t m_start; // meta
  uint64_t m_len; 
  uint64_t a_start; // block allocator
  uint64_t a_len; 
  uint64_t d_start; // data
  uint64_t d_len; 
  struct ThJournalDesc j_desc;
  struct ThMetaDesc    m_desc;
  struct ThAllocDesc   a_desc;
  struct ThDataDesc    d_desc;
  // need this to be persisent
  map<uint64_t, uint64_t> seg_location;
} __attribute__ ((packed));


// log-structured scheme
struct ThStartMarker {
  uint64_t s_mark = 0xffff;
};

struct ThEndMarker {
  uint64_t e_mark = 0xffff;
};

#if 0
struct ThChunk {
  bufferlist bl_data;
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
};
WRITE_CLASS_ENCODER(ThChunk)

struct ThMattr {
  bufferlist bl_xattr;
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
};
WRITE_CLASS_ENCODER(ThMattr)
#endif

struct ThChunkInfo {
public:
  hobject_t oid;
  uint64_t offset;
  uint64_t len;
  int d_index;
  int m_index;
  ThChunkInfo() {
    d_index = -1;
    m_index = -1;
  }
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
};
WRITE_CLASS_ENCODER(ThChunkInfo)

struct ThSegment {
public:
  struct ThStartMarker s_mark;

  uint64_t seg_id;
  uint64_t num_objects;
  mutable uint64_t seg_len;
  vector< struct ThChunkInfo > chunk_infos;
  mutable uint64_t chunk_start;
  int num_chunk;
  mutable uint64_t mattr_start;
  int num_mattr;

  bufferlist chunk_bl;
  bufferlist mattr_bl;

#if 0
  vector<struct ThChunk> chunks;
  vector<struct ThMattr> mattrs;
#endif
  struct ThEndMarker e_mark;
  ThSegment() {
    seg_id = 0;
    num_objects = 0;
    seg_len = 0;
    chunk_start = 0;
    num_chunk = 0;
    mattr_start = 0;
    num_mattr = 0;
  }
  void set_seg_len(uint64_t);
  void encode(bufferlist& bl) const;
  void decode(bufferlist::const_iterator& p);
};
WRITE_CLASS_ENCODER(ThSegment)
std::ostream& operator<<(std::ostream& out, const ThSegment &seg);

#endif

#if 0
  void encode(ceph::buffer::list& bl, uint64_t features) const;
  void decode(ceph::buffer::list::const_iterator& bl);
  void decode(ceph::buffer::list& bl) {
    auto p = std::cbegin(bl);
    decode(p);
  }
#endif
//} __attribute__ ((packed));

#if 0
  void encode(ceph::buffer::list& bl, uint64_t features) const;
  void decode(ceph::buffer::list::const_iterator& bl);
  void decode(ceph::buffer::list& bl) {
    auto p = std::cbegin(bl);
    decode(p);
  }
#endif
//} __attribute__ ((packed));
