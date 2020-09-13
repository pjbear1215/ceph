===============
 PoseidonStore
===============

Goals and Basics
======================

* Target only high-end NVMe devices with NVM (not concerned with ZNS devices)
* Designed entirely for low CPU consumption
    - Remove a black-box component like RocksDB and a file abstraction layer in BlueStore to avoid unnecessary overheads (e.g., data copy and serialization/deserialization)  
    - Make use of *io_uring*, new kernel asynchronous I/O interface, to selectively use the interrupt driven mode for CPU efficiency (or polled mode for low latency)
    - Hybrid update strategies for different data size (similar to BlueStore)
        - in-place update for small I/O to minimize CPU consumption by reducing host-side GC
        - out-of-place update for large I/O to eliminate double write
* Use Seastar futures programming model for a sharded data/processing model


Background
----------
Both in-place and out-of-place update strategies have their pros and cons. 

Log-structured based storage system is a typical example that adopts an update-out-of-place approach. It never modifies the written data. Writes always go to the end of the log. It enables I/O sequentializing.

* Pros
    - Higher random I/O performance for HDDs (and maybe for SSDs)
    - It doesn't require WAL to support transaction due to the no-overwrite nature (no double write)
    - Flash friendly (less GC activity for SSDs)
* Cons
    - It requires host-side GC that induces overheads
        - I/O amplification (host-side)
        - Host-CPU consumption

The update-in-place strategy has been used widely for conventional file systems such as ext4 and xfs. Once a block has been placed in a given disk location, it doesn't move. Thus, writes go to the corresponding location in the disk.

* Pros
    - Less host-CPU consumption (leave GC entirely up to SSDs)
* Cons
    - WAL is required to support transaction (double write to prevent partial write)
    - Flash unfriendly (more GC activity for SSDs)
    

Motivation and Key Ideas
----------

In modern distributed storage systems, a server node can be equipped with multiple 
NVMe storage devices. In fact, ten or more NVMe SSDs could be attached on a server.
As a result, it is hard to achieve NVMe SSD's full performance due to the limited CPU resources 
available in a server node. In such environments, CPU tends to become a performance bottleneck.
Thus, now we should focus on minimizing host-CPU consumption, which is the same as the Crimson's objective.

Towards a object store highly optimized for CPU consumption, three design choices have been made.

* **PoseidonStore does not have a black-box component like RocksDB in BlueStore.** Thus, it can avoid unnecessary data copy and serialization/deserialization overheads. Moreover, we can remove an unncessary file abstraction layer, which was required to run RocksDB. Object data and metadata is now directly mapped to the disk blocks (no more object-to-file mapping). Eliminating all these overheads will reduce CPU consumption.

* **PoseidonStore makes use of io_uring, new kernel asynchronous I/O interface to exploit interrupt-driven I/O.** User-space driven I/O solutions like SPDK provide high I/O performance by avoiding syscalls and enabling zero-copy access from the application. However, it does not support interrupt-driven I/O, which is only possible with kernel-space driven I/O. Polling is good for low-latency but bad for CPU efficiency. On the other hand, interrupt is good for CPU efficiency and bad for low-latency (but not that bad as I/O size increases). Note that network acceleration solutions like DPDK also excessively consume CPU resources for polling. Using polling both for network and storage processing aggrevates CPU consumption. Since network is typically much faster and has a higher priority than storage, polling should be applied only to network processing.

* **PoseidonStore uses hybrid update strategies for different data size, similar to BlueStore.** As we discussed, both in-place and out-of-place update strategies have their pros and cons. Since CPU is only bottlenecked under small I/O workloads, we chose update-in-place for small I/Os to miminize CPU consumption while choosing update-out-of-place for large I/O to avoid double write. Double write for small data may be better than host-GC overhead in terms of CPU consumption in the long run. Although it leaves GC entirely up to SSDs,
high-end NVMe SSD has enuough powers to handle more works. Also, SSD lifespan is not a practical concern these days (there is enough program-earse cycle limit [1]). On the other hand, for large I/O workloads, the host can afford process host-GC. Also, the host can garbage collect invalid objects more effectively when their size is large.

Data layout basics
----------
Disk space is divided into large segments (100's of MB). One or more cores/shards will access to a single NVMe SSD. Each shard will have a set of sharded partitions (SP). SP is a logical partition that can dynamically grown and shrunk. SP can get bigger by allocating segments and expanding address space, and can be smaller by returning segments. This design allows entire disk space to be efficiently utilized even where disk space load imbalance exists. All block numbers are SP relative. For the example below, assuming the segment size is 16KB and the block size is 4KB, the block number 5 is the second block in Segment 7.

.. ditaa::
       
       [Global Meta] | [SP 0 Meta] | [SP 1 Meta] | ... | [SP N Meta] | [Data]

       +---------------+-----------------------------+------------------------------------+
       |   Superblock  |        SP Metablock         |      SP Free Space Block           |
       | (Global Meta) |        (Per-SP Meta)        |          (Per-SP Meta)             |
       |               |                             |                                    |
       |               | +-------------------------+ |        # of free blocks            |
       |    # of SPs   | |   SP free space info    | |Free space B+tree root node block[2]| <- by count, by offset
       | # of free seg.| |   SP free space block   | |        # of allocated seg.         |
       | Segment bitmap| +-------------------------+ |      segment map[# of seg.]        |
       |               | |   SP onode B+tree info  | |      +--------------------+        |
       |               | |  B+tree root node block | |      |     Segment 0      |        |
       |               | +-------------------------+ |      |     Segment 7      |        |
       |               |                             |      |     Segment 3      |        |
       |               |                             |      +--------------------+        |
       +---------------+-----------------------------+------------------------------------+                    
       
       +-----------------------------------+
       | Free space B+tree root node block |
       |          (Per-SP Meta)            |
       |                                   |
       |           # of records            |
       |    left_sibling / right_sibling   |
       | +--------------------------------+| 
       | | keys[# of records]             ||
       | | +-----------------------------+||
       | | |   startblock / blockcount   |||
       | | |           ...               |||
       | | +-----------------------------+||
       | +--------------------------------||
       | +--------------------------------+| 
       | | ptrs[# of records]             ||
       | | +-----------------------------+||
       | | |       SP block number       |||
       | | |           ...               |||
       | | +-----------------------------+||
       | +--------------------------------+|
       +-----------------------------------+

       +-----------------------------------+  +----------------------------------+
       |    onode B+tree root node block   |  |           onode block            | 
       |          (Per-SP Meta)            |  |             (Data)               |
       |                                   |  |                                  |
       |           # of records            |  | +------------------------------+ |
       |    left_sibling / right_sibling   |  | |          Metadata            | |
       | +--------------------------------+|  | +------------------------------+ |
       | | keys[# of records]             ||  | |          inline data         | |
       | | +-----------------------------+||  | +------------------------------+ |
       | | |    start onode ID           |||  | |            xattrs            | |
       | | |           ...               |||  | +------------------------------+ |
       | | +-----------------------------+||  | |  omap B+tree root node block | | <--- by key string in lexicographical order
       | +--------------------------------||  | +------------------------------| | 
       | +--------------------------------+|  | |Extent B+tree root node block | | <--- by offset
       | | ptrs[# of records]             ||  | +------------------------------+ |
       | | +-----------------------------+||  +----------------------------------+
       | | |       SP block number       |||
       | | |           ...               |||
       | | +-----------------------------+||
       | +--------------------------------+|
       +-----------------------------------+


Observation
-----------

Three data types in Ceph
- Data (object data)
  - The cost of double write is high
  - The best mehod to store this data is in-place update
    - At least two operations required to store the data: 1) data and 2) location of 
      data. Nevertheless, a constant number of operations would be better than out-of-place
      even if it aggravates WAF in SSDs
- Metadta or small data (e.g., object_info_t, snapset, pg_log, and collection)
  - Multiple small-sized metadta entries for an object
  - The best solution to store this data is WAL + Using cache
    - The efficient way to store metadata is to merge all metadata related to data
      and store it though a single write operation even though it requires background
      flush to update the data partition


Design
======
.. ditaa::

  +-WAL partition-|----------------------Data partition---------------------------+
  | Sharded partition 1
  +-------------------------------------------------------------------------------+
  | WAL -> |      | Super block | Freelist info | Onode B+free info| Data blocks  |
  +-------------------------------------------------------------------------------+
  | Sharded partition 2
  +-------------------------------------------------------------------------------+
  | WAL -> |      | Super block | Freelist info | Onode B+free info| Data blocks  |
  +-------------------------------------------------------------------------------+
  | Sharded partition N 
  +-------------------------------------------------------------------------------+
  | WAL -> |      | Super block | Freelist info | Onode B+free info| Data blocks  |
  +-------------------------------------------------------------------------------+
  | Global information                                                           
  +-------------------------------------------------------------------------------+
  | Global WAL -> |                                                               |
  +-------------------------------------------------------------------------------+


- WAL
  - Log and frequently updated metadata are stored as a WAL entry in the WAL partition
  - Space within the WAL partition is continually reused in a circular manner
  - Flush the WAL entries if necessary
- Disk layout
  - Data blocks are metadata blocks or data blocks
  - Freelist manages the Root of free space B+free
  - Super block contains management info for a data partition


I/O procedure
-------------
- Write
  For incoming writes, data is handled differently depending on the request size; 
  data is either written twice (WAL) or written in a log-structured manner.

  (1) If Request Size ≤ Threshold (similar to minimum allocation size in BlueStore)
    Write data and metadata to [WAL] —flush—> Write them to [Data section (in-place)] and 
    [Metadata section], respectively.

    Since the CPU becomes the bottleneck for small I/O workloads, in-place update scheme is used.
    Double write for small data may be better than host-GC overhead in terms of CPU consumption 
    in the long run

  (2) Else if Request Size > Threshold
    Append data to [Data section (log-structure)] —> Write the corresponding metadata to [WAL] 
    —flush—> Write the metadata to [Metadata section]

    For large I/O workloads, the host can afford process host-GC
    Also, the host can garbage collect invalid objects more effectively when their size is large

    Note that Threshold can be configured to a very large number so that only the scenario (1) occurs.
    With this design, we can control the overall I/O procedure with the optimizations for crimson
    as described above.

  - Detailed flow
    - 1. Append a log entry that contains pg_log, snapset, object_infot_t, block allocation
      using NVMe atomic write command on the WAL
      - NVMe provides atomicity guarantees for a write command (Atomic Write Unit Power Fail)
        For example, 512 Kbytes of data can be atomically written at once without fsync()
      - Small size (object_info_t, snapset, etc.) can be embed
      - stage 1
        - if the data is small
        WAL (written) --> | TxBegin A | Log Entry | TxEnd A | 
        - if the data is large
        Data partition (written) --> | Data blocks | 
      - stage 2
        - if the data is small
        No need.
        - if the data is large
        Then, append the metadata to WAL.
        WAL --> | TxBegin A | Log Entry | TxEnd A | 

- Read
  - Use the cached object metadata to find out the data location
  - If not cached, need to search WAL after checkpoint and Object meta partition to find the 
    latest meta data

- Flush
  - Flush WAL entries which have committed. There are two conditions
    (1. the size of WAL is close to full, 2. a signal to flush).
    We can mitigate the overhead of frequent flush via batching processing, but it leads to
    delaying completion.




Crash consistency
------------------
Large case
- 1. Crash occurs right after writing Data blocks
  - Data partition --> | Data blocks |
  - We don't need to care this case. Data is not alloacted yet in reality. The blocks will be reused.
- 2. Crash occurs right after WAL 
  - Data partition --> | Data blocks |
  - WAL --> | TxBegin A | Log Entry | TxEnd A |
  - Write procedure is completed, so there is no data loss or inconsistent state

Small case
- 1. Crash occurs right after writing WAL
  - WAL --> | TxBegin A | Log Entry| TxEnd A |
  - All data has been written


Comparison
----------
- Best case (pre-allocation)
  - Only need writes on both WAL and Data partition without updating object metadata (for the location).
- Worst case 
  - At least three writes are required additionally on WAL, object metadata, and data blocks.
  - If the flush from WAL to the data parition occurs frequently, b+tree onode structure needs to be update
    in many times. To minimize such overhead, 

- WAL needs to be flushed if the WAL is close to full or a signal to flush.
  - The premise behind this design is OSD can manage the lateset metadta as a single copy. So,
    appended entires are not to be read
- Either best of worst case does not produce severe I/O amplification (it produce I/Os, but I/O rate is constant) 
  unlike LSM-tree DB (proposed design is similar to LSM-tree which has only level-0)


Detailed Design 
===============

- Onode lookup
  - Radix tree
  Our design is based on the prefix tree. Ceph already makes use of the chracteristic of OID's prefix to split or search
  the OID (e.g., pool id + hash + oid). So, the prefix tree fits well to store or search the object. Our scheme is designed 
  to lookup the prefix tree efficiently.

  - Sharded partition
  A few bits at the begining of OID determine a sharded partition where the object is located.
   +--------------------------+--------------------------+--------------------------+
   | sharded partition 1 (00) | sharded partition 2 (01) | sharded partition 3 (10) |
   +--------------------------+--------------------------+--------------------------+ 

  - Ondisk onode
  stuct onode {
    radix tree childs;
    radix tree parent_node;
    extent tree block_maps;
    clone tree clones;
    omap lists omaps;
  }
  onode contains the radix tree for lookup, which means we can search objects uinsg tree information in onode 
  if we know the onode. Also, if the data size is small, the onode can embed the data.
  The onode has fixed size. On the other hands, block_maps, clones have variable-length.
   +---------------+------------+--------+-------+
   | on-disk onode | block_maps | clones | omaps |
   +---------------+------------+--------+-------+ 
              |           ^        ^
              |-----------|--------|

  - Lookup
  The location of the root of onode tree is specified on Onode B+tree info, so we can find out where the object 
  is located by using the prefix tree. For example, shared partition is determined by OID as described above. 
  Using rest of the OID's bits and radix tree, lookup procefure finds out the location of the onode.
  The extent tree (block_maps) contains where data chunks locate by using extent map, so we finally figure out the data location.


- Allocation
  - Sharded partitions
  Entire disk space is divided into a number of equally sized chunks called sharded partition (SP).
  Each SP has its own data structures to manage the disk partition.

  - Data allocation
  As we explained above, the only management infos (e.g., super block, freelist info, onode b+tree info) are pre-allocated 
  in each shared partition. Given OID, we can map any data to the extent tree in the node. 
  Blocks can be allocated by searching the free space tracking data structure (we explain below).

  - Free space tracking
  The freespace is tracked on a per-AG basis. We can use extent-based B+tree in XFS for free space tracking.
  The freelist info contains the root of free space B+free.

- Omap
In this design (see below figure), omap data is tracked by lists in onode. the onode only has the header of omap.
The header contains entires which indicate where the name list and data list exist.
So, if we know the onode, omap can be easliy found via omap lists.

- Fragmentation
  - Internal fragmentation
  We pack different type of data/metadata in a single block as many as possible to reduce internal fragmentation.
  Extent-based B+tree may help reduce this further by allocating contiguous blocks that best fit for the object

  - External fragmentation
  Frequent object create/delete may lead to external fragmentation
  In this case, we need cleaning work (GC-like) to address this.
  For this, we are referring the NetApp’s Continuous Segment Cleaning, which seems similar to the SeaStore’s approach
  Countering Fragmentation in an Enterprise Storage System (NetApp, ACM TOS, 2020)

.. ditaa::


       +---------------+-------------------+-------------+
       | Freelist info | Onode B+tree info | Data blocks | ------|
       +----+----------+-------------------+------+------+       |   
            ---------------------|           |                   |   
            |        OID                     |                   |     
            |                                |                   |     
        +---+---+                            |                   |     
        | Root  | Radix tree                 |                   |     
        +---+---+                            |                   |     
            |                                |                   |     
            v                                |                   |   
       +---------+---------+---------+       |                   |     
       | Subtree | ...     | Subtree |       |                   v     
       +=========+=========+=========+       |      +---------------+       
       | onode   | ...     | ...     |       |      |               |
       +---------+---------+---------+       |      | Num Chunk     |        
    +--| onode   | ...     | ...     |       |      | <Offset, len> |        
    |  +---------+---------+---------+       |      | <Offset, len> |-------|
    |                                        |      | ...           |       | 
    |                                        |      +---------------+       | 
    |                                        |      ^                       |
    |                                        |      |                       |
    |                                        |      |                       |   
    |                                        |      |                       |   
    |  +---------------+  +-------------+    |      |                       v   
    +->| leafnode      |  | leafnode    |<---|      |       +------------+------------+  
       +===============+  +=============+           |       | Block0     | Block1     |  
       | OID           |  | OID         |           |       +============+============+  
       | Omaps         |  | Omaps       |           |       | Data       | Data       |  
       | Clones        |  | Clones      |           |       +------------+------------+  
       | Data Extent   |  | Data Extent |-----------|     
       +---------------+  +-------------+           


WAL
---
Each AG has a WAL.
The datas written to the WAL are all metadta updates, free space update and data.
Note that only data smaller than the predefined threshold needs to be written to the WAL.
The larger data is written to the unallocated free space and its onode's extent_map is updated accordingly 
(also on-disk extent map). We statically allocate WAL partition aside from data partition pre-configured.


Partition
---------
Initially, PoseidonStore employs static allocation of partition. The number of sharded partitions
is fixed and the size of each partition also should be configured before running cluster.


Cache
-----
There are mainly two cache data structures; onode cache and block cache.
It looks like below.

1.Onode cache:
map <OID, OnodeRef>;
Onode {
  extent tree block_maps;
  clone tree clones;
  omap lists omaps;
}
2. Block cache:
Omap cache --> map <OID, <omap_key, value>>
Data cache --> map <OID, <extent, value>>

To fill the onode data structure, object meta and omap of the target object can be retrieved 
using the prefix tree.
Block cache is used for caching a block contents. For a transaction, all the updates to blocks 
(including object meta block, allocation bitmap block, data block) 
are first performed in the in-memory block cache.
After writing a transaction to the WAL, the dirty blocks are flushed to their respective locations in the 
respective partitions.
PoseidonStore can configure cache size for each type. Simple LRU cache eviction strategy can be used for both.


Sharded partitions (with cross-SP transaction)
--------------------------------------------
Entire disk space is divided into a number of equally sized chunks called sharded partitions (SP).
A collection is stored into an SP, not across SPs.
The prefixes of the parent collection ID (original collection ID before collection splitting. That is, hobject.hash) 
is hashed to map any collections to SPs.
We can use BlueStore's approach for collection splitting, changing the number of significant bits for the collection prefixes.
Because the prefixes of the parent collection ID do not change even after collection splitting, the mapping between 
the collection and SP is maintained.
The number of SPs may be configured to match the number of CPUs allocated for each disk so that each SP can hold 
a number of objects large enough for cross-SP transaction not to occur.

In case of need of cross-SP transaction, we could use the global WAL (acquire the source SP and target SP locks before 
processing the cross-SP transaction). If SPs have an entry in the global WAL, it should apply it as soon as possible, then
remove it from the gobal WAL.

For the load unbalanced situation, we adjust the mapping between extent tree and data blocks in other SPs (This allocation
probably requires cross-SP transaction). 


Discussion
==========
ToDo


[1] Stathis Maneas, Kaveh Mahdaviani, Tim Emami, Bianca Schroeder:
A Study of SSD Reliability in Large Scale Enterprise Storage Deployments. FAST 2020: 137-149
