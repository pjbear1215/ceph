
#include "thinstore_types.h"
#include "common/Formatter.h"
#include "common/Checksummer.h"
#include "include/stringify.h"

#define MIN_ENTRY_SIZE 512

//void ThJournalEntry::encode(ceph::buffer::list& bl, uint64_t feat) const
void ThJournalEntry::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  encode(oid, bl);
  encode(nid, bl);
  encode(op, bl);
  encode(offset, bl);
  encode(length, bl);
  encode(alloc_no_begin, bl);
  encode(alloc_no_end, bl);
  int remain = MIN_ENTRY_SIZE - bl.length(); 
  if (remain > 0) {
    bl.append_zero(remain);
  }

}

void ThJournalEntry::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  decode(oid, bl);
  decode(nid, bl);
  decode(op, bl);
  decode(offset, bl);
  decode(length, bl);
  decode(alloc_no_begin, bl);
  decode(alloc_no_end, bl);
}

uint32_t ThJournalEntry::get_size() {
  using ::ceph::encode;
  bufferlist bl;
  encode(oid, bl);
  encode(nid, bl);
  encode(op, bl);
  encode(offset, bl);
  encode(length, bl);
  encode(alloc_no_begin, bl);
  encode(alloc_no_end, bl);

  uint32_t length = bl.length();
  uint32_t least_size = 2;
  while (length > least_size) {
    least_size = least_size * 2;
  }

  if (least_size < MIN_ENTRY_SIZE) {
    least_size = MIN_ENTRY_SIZE;
  }

  aligned_size = least_size;
  return least_size;
}

void ThMetaEntry::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  //encode(oid, bl);
  encode(nid, bl);
  encode(size, bl);
  // ToDo
  encode(has_block_map, bl);
  encode(oid, bl);

  // ToDo: flush block_maps ?
  if (block_maps && need_flush_block_map) {
      encode(*block_maps, bl);
  }
  encode(encode_flag, bl);
  encode(aligned_size, bl);
  encode(mattrs_seg_id, bl);

  uint32_t length = bl.length();
  if (aligned_size > length) {
    uint32_t remain = aligned_size - length;
    bl.append_zero(remain);
  } else {
    ceph_assert(0);
  }
  
}

void ThMetaEntry::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  //decode(oid, bl);
  try {
    decode(nid, bl);
    decode(size, bl);
    decode(has_block_map, bl);
    decode(oid, bl);
    need_flush_block_map = false;
    // ToDo
    if (has_block_map) {
      block_maps = new ThMetaBlockMap(block_size);
      decode(*block_maps, bl);
    }
    decode(encode_flag, bl);
    decode(aligned_size, bl);
    decode(mattrs_seg_id, bl);
  } catch (...) {
    //dout(0) << __func__ << " no objects ? " << dendl;
  }
}
#if 0
    for (int i = 0; i < NUMBER_OF_BLOCK_MAPS; i++) {
      encode(block_maps->offset[i], bl);
    }
    for (int i = 0; i < NUMBER_OF_BLOCK_MAPS; i++) {
      encode(block_maps->block_maps[i], bl);
    }
#endif
#if 0
      for (uint64_t i = 0; i < NUMBER_OF_BLOCK_MAPS; i++) {
	decode(block_maps->block_maps[i], bl);
      }
      //decode(block_maps->block_maps, bl);
      for (int i = 0; i < NUMBER_OF_BLOCK_MAPS; i++) {
	decode(block_maps->block_maps[i], bl);
      }
#endif

uint32_t ThMetaEntry::get_size() {
  using ::ceph::encode;
  bufferlist bl;
  //encode(oid, bl);
  encode(nid, bl);
  encode(size, bl);
  encode(has_block_map, bl);
  encode(oid, bl);
  // ToDo

  uint32_t length = bl.length();
  struct ThMetaBlockMap block_map(block_size);
  length += block_map.get_size();
  uint32_t least_size = 2;
  while (length > least_size) {
    least_size = least_size * 2;
  }
  if (least_size < MIN_ENTRY_SIZE) {
    least_size = MIN_ENTRY_SIZE;
  }
  aligned_size = least_size;
  return least_size;
}

ostream& operator<<(ostream& out, const ThMetaEntry &en)
{
  out << " nid " << en.nid << " size " << en.size
      << " block_size " << en.block_size
      << " has_block_map " << en.has_block_map
      << " cached " << en.cached
      << " flush_block_map " << en.need_flush_block_map
      << " encode_flag " << en.encode_flag
      << " aligned_size " << en.aligned_size
      << " mattr_seg_id " << en.mattrs_seg_id
      << " ) (";
  if (en.block_maps) {
    out << en.block_maps;
  }
  out << ")";
  return out;
}

#if 0
void ThMattr::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  encode(block_offset, bl);
}

void ThMattr::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  try {
    decode(block_offset, bl);
  } catch (...) {
    //dout(0) << __func__ << " no objects ? " << dendl;
  }
}

void ThChunk::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  encode(bl_data, bl);
}

void ThChunk::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  try {
    decode(bl_data, bl);
  } catch (...) {
    //dout(0) << __func__ << " no objects ? " << dendl;
  }
}
#endif

void ThMetaBlockMap::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  encode(block_offset, bl);
  encode(block_size, bl);
}

void ThMetaBlockMap::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  try {
    decode(block_offset, bl);
    decode(block_size, bl);
  } catch (...) {
    //dout(0) << __func__ << " no objects ? " << dendl;
  }
}

ostream& operator<<(std::ostream& out, const ThMetaBlockMap &map)
{
  out << " meta block map size " << map.block_offset.size() 
      << " block_size " << map.block_size
      << " first allocation " << *(map.block_offset.begin())
      << " last allocation " << *(map.block_offset.rbegin());
  return out;
}

void ThChunkInfo::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  encode(oid, bl);
  encode(offset, bl);
  encode(len, bl);
  encode(d_index, bl);
  encode(m_index, bl);
}

void ThChunkInfo::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  try {
    decode(oid, bl);
    decode(offset, bl);
    decode(len, bl);
    decode(d_index, bl);
    decode(m_index, bl);
  } catch (...) {
    //dout(0) << __func__ << " no objects ? " << dendl;
  }
}

void ThSegment::encode(bufferlist& bl) const
{
  using ::ceph::encode;
  //encode(oid, bl);
  encode(seg_id, bl);
  encode(s_mark.s_mark, bl);
  encode(num_objects, bl);
  ceph_assert(num_objects == chunk_infos.size());
  for (auto p : chunk_infos) {
    encode(p, bl);
  }
  encode(num_chunk, bl);
  encode(num_mattr, bl);

#if 0
  ceph_assert(num_chunk == chunks.size());
  ceph_assert(num_mattr == mattrs.size());
#endif

  encode(e_mark.e_mark, bl);

  seg_len = bl.length() + sizeof(uint64_t)*3; 
  uint32_t remain_meta = seg_len % ALIGN_SIZE;
  uint32_t padding_meta = 0;
  if (remain_meta != 0) {
    padding_meta = ALIGN_SIZE - remain_meta;
  }
  seg_len += padding_meta;
  chunk_start = seg_len;
  mattr_start = seg_len + chunk_bl.length();
  seg_len = mattr_start + mattr_bl.length();


  uint32_t remain_data = seg_len % ALIGN_SIZE;
  uint32_t padding_data = 0;
  if (remain_data != 0) {
    padding_data = ALIGN_SIZE - remain_data;
  }
  seg_len += padding_data;

  encode(chunk_start, bl);
  encode(mattr_start, bl);
  encode(seg_len, bl);
  
  bl.append_zero(padding_meta);
  bl.append(chunk_bl);
  bl.append(mattr_bl);
  bl.append_zero(padding_data);

#if 0
  for (auto p : chunks) {
    encode(p, bl);
  }
  for (auto p : mattrs) {
    encode(p, bl);
  }
#endif
  
}

//void ThMetaEntry::decode(ceph::buffer::list::const_iterator& bl)
void ThSegment::decode(bufferlist::const_iterator& bl)
{
  using ::ceph::decode;
  //decode(oid, bl);
  try {
    decode(seg_id, bl);
    decode(s_mark.s_mark, bl);
    decode(num_objects, bl);
    decode(seg_len, bl);
    for (int i = 0; i < num_objects; i++) {
      struct ThChunkInfo temp;
      decode(temp, bl);
      chunk_infos.push_back(temp);
    }

    decode(num_chunk, bl);
    decode(num_mattr, bl);

    decode(e_mark.e_mark, bl);

    decode(chunk_start, bl);
    decode(mattr_start, bl);
    decode(seg_len, bl);
  
    // do not decode data and mattrs
#if 0
  bl.append_zero(padding_meta);
  bl.append(chunk_bl);
  bl.append(mattrs_bl);
  bl.append_zero(padding_data);
#endif
  } catch (...) {
    //dout(0) << __func__ << " no objects ? " << dendl;
  }
}

ostream& operator<<(ostream& out, const ThSegment &seg)
{
  out << " seg_id " << seg.seg_id << " num_objects " << seg.num_objects
      << " seg_len " << seg.seg_len;
  out << ") (";
  for (auto p : seg.chunk_infos) {
    out << " oid: " << p.oid << " offset: " << p.offset
	<< " len: " << p.len << " d_index: " << p.d_index
	<< " m_index: " << p.m_index;
  }
  out << ")";
  out << " chunk_start: " << seg.chunk_start << " num_chunk: " << seg.num_chunk
      << " mattrs_start: " << seg.mattr_start << " num_mattrs: " << seg.num_mattr
      << " chunk length " << seg.chunk_bl.length() 
      << " mattrs length " << seg.mattr_bl.length();
  return out;
}

