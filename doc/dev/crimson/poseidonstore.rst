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
    

Motivation
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
  - Most metadata can be cached in memory because the object size is large (4MB)
  - Multiple small-sized metadta entries for an object
  - The best solution to store this data is WAL + Using cache
    - The efficient way to store metadata is to merge all metadata related to data
      and store it though a single write operation even though it requires background
      flush to update the data partition

Design
======
.. ditaa::

  +-WAL partition-|----------------------Data partition-------------------------+
  | Sharded partition 1
  +-----------------------------------------------------------------------------+
  | WAL -> |      | Super block | Object meta | Allocation bitmap | Data blocks |
  +-----------------------------------------------------------------------------+
  | Sharded partition 2
  +-----------------------------------------------------------------------------+
  | WAL -> |      | Super block | Object meta | Allocation bitmap | Data blocks |
  +-----------------------------------------------------------------------------+
  | Sharded partition N 
  +-----------------------------------------------------------------------------+
  | WAL -> |      | Super block | Object meta | Allocation bitmap | Data blocks |
  +-----------------------------------------------------------------------------+


- WAL
  - Log and frequently updated metadata are stored as a WAL entry in the WAL partition
  (Can be placed on NVM)
  - Space within the WAL partition is continually reused in a circular manner
  - Flush the metadata if necessary
- Write procedure for Metadata
  - Appended at the WAL first
  - Overwrite the metadta in the data partition when flushing
- Write procedure for Data
  - Overwrite the data in the data partition
  - Disk layout
  - Object meta can embed data. For example, object_info_t can be recorded as an entry of
    the Object meta
  - Allocation bitmap manages the Data blocks
  - Super block manages data partitions


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
      - Small size (object_info_t, snapset, collection, etc.) can be embed
            - stage 1
        WAL (written) --> | TxBegin A | Log Entry | TxEnd A | (if small)
        Data partition (written)--> | Data blocks | (if large)
          - 2. Then, append the data to WAL (if large)
      - stage 2 (Updating object meta and allocation bitmap can be skipped via pre-allocation)
        WAL --> | TxBegin A | Log Entry | TxEnd A | (if large)

- Read
  - Use the cached object metadata to find out the data location
  - If not cached, need to search WAL after checkpoint and Object meta partition to find the 
    latest meta data

- Flush
  - Flush WAL entries whenever needed


Crash consistency
------------------
Large case
- 1. Crash occurs right after writing Data blocks
  - Data partition --> | Data blocks |
  - We don't need to care this case. Data is not alloacted yet in reality. The blocks will be reused.
- 2. Crash occurs right after WAL using atomic write command
  - WAL --> | TxBegin A | Log Entry| TxEnd A |
  - Data partition --> | Data blocks |
  - Write procedure is completed, so there is no data loss or inconsistent state

Small case
- 1. Crash occurs right after writing WAL
  - WAL --> | TxBegin A | Log Entry| TxEnd A |
  - All data has been written


Comparison
----------
- Best case (pre-allocation)
  - Only need two writes on both WAL and Data partition without updating Object meta and Data bitmap
- Worst case (flush happens)
  - Without embedding, at least three writes are required additionally on Object meta, Data bitmap, and Data blocks
  - WAL needs to be flushed if the WAL is close to full and the WAL entry is snapset, object_info_t,
    collection and pg_log
  - The premise behind this design is OSD can manage the lateset metadta as a single copy. So,
    appended entires are not to be read
- Either best of worst case does not produce severe I/O amplification (it produce I/Os, but I/O rate is constant) 
  unlike LSM-tree DB (proposed design is similar to LSM-tree which has only level-0)


Detailed Design 
===============

- Onode lookup
 - Flat namespace
  Object data, object meta, omap, and xattrs are all the data related to an object (we call it a head object from ghobject).
  PoseidonStore adds different suffixes to the head object ID to create PSD_OID to distinguish them 
  (e.g., Assuming head object ID = 12.e4 -> Object data's PSD_OID = 12.e4_data, Object meta's PSD_OID = 12.e4_meta, omap's 
  PSD_OID = 12.e4_omap_key1, xattr's PSD_OID = 12.e4_xattr_key1).
  By eliminating suffixes, we can recover their head object ID. Thus, we can know they are all related to the same object.
  Note that because all objects are sorted by their PSD_OIDs in lexicographic order, xattrs and omap can be iterated
  in lexicographic order. For object deletion, all the objects having the same head object ID are searched and deleted.

  A leafnode contains <OID, Block no, Offset, Extent>.
  With the hash entry, we can find the disk location of the target object.
  Since the Extent entry in the Block contains <OID, Chunk Number, (Offset, len), (Offset, len), … >, 
  we can read the object.

 -Lookup
  As described in the figure, we replaced hash table on Object meta with b+tree to find out items efficiently.
  This is because we can control the tree based on the policy we define and it can do fast lookup.
  There is no on-disk onode data structure. To fill the in-memory onode data structure, object meta, xattrs,
  and omap of the target object can be retrieved using the respective PSD_OIDs.
  Also, we use PSD_OID as a key to find the leaf node that contains extent.
  Data type in leaf node indicates if the pointing data is inline data or an extent array of <start block num, block count>.
  According to the data type, it reads the blocks needed for the object.
  If you look at the figure as below, there is the leafnode which contains extent info. Using
  extent info, we can know where the extent is located. Extent contains where data
  chunks locate by using extent map, so we finally figure out the data location.


- Allocation
Allocation Groups
Entire disk space is divided into a number of equally sized chunks called Allocation Groups (AG).
Each reactor thread in crimson-osd is responsible for a set of AGs (# of AGs / # of threads).
Each AG has its own data structures to manage the disk partition.

The freespace is tracked on a per-AG basis.
The initial version of PoseidonStore will use bitmaps for the validity of all blocks in the AG.
The bitmaps are pre-allocated in the allocation bitmap partition.
Upon new object write, we allocate an extent of contiguous blocks large enough to fit data by referring to the allocation bitmaps.
We may use two extent-based b+trees for efficient contiguous free space tracking; one by block number and another 
by block count (similar to XFS).
We leave this for future work. The means of free space tracking can be configured at the initial disk setup 
(cannot be changed after configuration).

- Data allocation
As we explained above, the entire hash table is pre-allocated in each AG. Given OID, we can map any data to 
the segment and offset. Blocks can be allocated by searching the free space 
tracking data structure (we explain below).
Based on onode lookup, we probably know where OID is located via Block number. 

- Free space tracking
We can use either data block bitmap in EXT4 or extent-based B+tree in XFS for free space tracking.
Our first prototype will be implemented based on the block bitmap for the sake of implementation.
Regarding allocation bitmap, we have a plan to re-use the existing the design of bitmapfreelistmanager and bitmapallocator 
in Bluestore (Underlying storage should be changed KV db to raw device)  

- Omap
In this design (see below figure), omap is not different from data and onode. They all can be retrieved via hash table lookup

- Fragmentation
Internal fragmentation
We pack different type of data/metadata in a single block as many as possible to reduce internal fragmentation.
Extent-based B+tree may help reduce this further by allocating contiguous blocks that best fit for the object

External fragmentation
Frequent object create/delete may lead to external fragmentation
In this case, we need cleaning work (GC-like) to address this.
For this, we are referring the NetApp’s Continuous Segment Cleaning, which seems similar to the SeaStore’s approach
Countering Fragmentation in an Enterprise Storage System (NetApp, ACM TOS, 2020)

.. ditaa::


       +-------------+-------------------+-------------+
       | Object meta | Allocation bitmap | Data blocks | --------------------------------
       +----+--------+-------------------+------+------+               |                |
            |                                                          |                | 
            | OID                                                      |                | 
            |                                                          |                |
        +---+---+                                                      |                |
        | Root  |                                                      |                |
        +---+---+                                                      |                |
            |                                                          |                |
            v                 Tree                                     |                |
       +---------+---------+---------+                                 |                |
       | Subtree | ...     | Subtree |                                 v                |
       +=========+=========+=========+                          +---------------+       |
       | Header  | ...     | ...     |                          | OID           |<-----------------+
       +---------+---------+---------+                          | Num Chunk     |       |          |
    +--| Entry0  | ...     | ...     |                          | <Offset, len> |       |          |
    |  +---------+---------+---------+                          | <Offset, len> |-------|----+     |
    |  | ...     | ...     | ...     |                          | ...           |       |    |     |
    |  +---------+---------+---------+                          +---------------+       |    |     |
    |  | EntryK  | ...     | ...     |                                                  |    |     |
    |  +---------+---------+---------+                                                  |    |     |
    |                                                                                   |    |     |
    |                                                                                   |    |     |
    |  +---------------+  +----------+  +-------------+                                 v    v     |
    +->| leafnode      |  | leafnode |  | leafnode    |               +------------+------------+  |
       +===============+  +==========+  +=============+               | Block0     | Block1     |  |
       | OID           |  | OID      |  | OID         |               +============+============+  |
       | Block no      |  | Block no |  | Block no    |               | Free       |            |  |
       | Offset        |  | Offset   |  | Offset      |               +------------+            +  |
       | Extent        |  | Extent   |  | Extent      |----------+    | Small data | Data       |  |
       +---------------+  +----------+  +-------------+          |    +------------+            +  |
                                                                 +-+->| Extent     |            |  |
                                                                      +------------+------------+  |
                                                                          |                        |
                                                                          +------------------------+


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
Since PoseidonStore does not have the on-disk onode strucutre, there needs to be in-memory onode structure 
for fast tracking of the objects.

It looks like below.

Onode {
  metadata
  xattr map <key, value>
  omap <key, value>
  extent_map
}

To fill the in-memory onode data structure, object meta, xattrs, and omap of the target object can be retrieved 
using the respective PSD_OIDs.
Block cache is used for caching a block contents and supporting transactions.
For a transaction, all the updates to blocks (including object meta block, allocation bitmap block, data block) 
are first performed in the in-memory block cache.
After writing a transaction to the WAL, the dirty blocks are flushed to their respective locations in the 
respective partitions.
PoseidonStore can configure cache size for each type. Simple LRU cache eviction strategy can be used for both.


Allocation Group (with cross-AG transaction)
--------------------------------------------
Entire disk space is divided into a number of equally sized chunks called Allocation Groups (AG).
Each reactor thread in crimson-osd is responsible for a set of AGs (# of AGs / # of threads).
Each AG has its own data structures to manage the disk partition.
A collection is stored into an AG, not across AGs.
The prefixes of the parent collection ID (original collection ID before collection splitting. That is, hobject.hash) 
is hashed to map any collections to AGs allocated for the reactor thread.
We can use BlueStore's approach for collection splitting, changing the number of significant bits for the collection prefixes.
Because the prefixes of the parent collection ID do not change even after collection splitting, the mapping between 
the collection and AG is maintained.
The number of AGs may be configured to match the number of CPUs allocated for each disk so that each AG can hold 
a number of objects large enough for cross-AG transaction not to occur.
In case of need of cross-AG transaction, we could simply use the per-AG lock (acquire the source and target locks before 
processing the cross-AG transaction).
For the load unbalanced situation, we adjust the mapping between collection and AG and the mapping between reactor 
thread and collection. 


Discussion
==========
ToDo


[1] Stathis Maneas, Kaveh Mahdaviani, Tim Emami, Bianca Schroeder:
A Study of SSD Reliability in Large Scale Enterprise Storage Deployments. FAST 2020: 137-149
