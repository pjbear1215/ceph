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
- Log (e.g., pglog)
  - Can be deleted after checkpoint
  - The best method to store this data is circular log (WAL)
- Data (object data)
  - Ojbect size tends to be large
  - The cost of double write is high
  - The best mehod to store this data is in-place update
    - At least two operations required to store the data: 1) data and 2) location of 
      data. Nevertheless, constant number of operations would be better than out-of-place
      even if it aggravates WAF in SSDs
- Metadta (e.g., object_info_t, snapset, andl collection)
  - Most metadta can be cached in memory because the object size is large (4MB)
  - Multiple small-sized metadta entries for an object
  - The best solution to store this data is WAL + Using cache
    - The efficient way to store metadta is to merge all metadata related to data
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
  - Space within the WAL partition is continually reused in a curcular manner
  - Flush the metadta if necessary
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
  - 1. Append a log entry that contains pg_log, snapset, object_infot_t, block allocation
    using NVMe atomic write command on the WAL
    - NVMe provides atomicity guarantees for a write command (Atomic Write Unit Power Fail)
      For example, 512 Kbytes of data can be atomically written at once without fsync()
    - Small size (object_info_t, snapset, collection, etc.) can be embed
    - stage 1
      WAL (written) --> | TxBegin A | Log Entry |
  - 2. Then, overwrite the data to data blocks
    - stage 2 (Updating object meta and allication bitmap can be skipped via pre-allocation)
      WAL --> | TxBegin A | Log Entry |  
      Data partition (written)--> | Object Meta | Allocation bitmap | Data blocks |
  - 3. Append TxEnd with a next log entry when doing next write
    - stage 3
      WAL (written) --> | TxBegin A | Log Entry | TxEnd A | TxBegin B | LogEntry |

- Read
  - Use the cached object metadata to find out the data location
  - If not cached, need to search WAL after checkpoint and Object meta partition to find the 
    latest meta data

- Flush
  - Flush WAL entries whenever needed


Recovery procedure
------------------
- In-place update may lead to partial write to data 
  - We can recover data from relicas in the case of inconsistency (we need to detect it)

- 1. Crash occurs right after writing WAL
  - WAL --> | TxBegin A | Log Entry |
  - We don't know if data is written to data blocks
  - Send pull request to replicas to get the latest data
- 2. Crash occurs right after writing Data
  - WAL --> | TxBegin A | Log Entry|  
  - Data partition --> | Object Meta | Allocation bitmap | Data blocks |
  - We don't know if data written to data blocks even though data is stored in reality
  - Send pull request to replicas to get the latest data
- 3. Crash occurs after a next write
  - WAL --> | TxBegin A | Log Entry | TxEnd A | TxBegin B | LogEntry |
  - Abandon failed write and recover previous state that contains Tx A


Comparison
----------
- Best case (pre-allocation)
  - Only need two writes on both WAL and Data partition wihout updating Object meta and Data bitmap
- Worst case (flush happens)
  - Without embedding, at leat three writes are requrred additionally on Object meta, Data bitmap, and Data blocks
- WAL needs to be flushed if the WAL is close to full and the WAL entry is snapset, object_info_t 
  and collection excepts for pg_log
  - The premise behind this design is OSD can manage the lateset metadta as a single copy. So,
    appended entires are not to be read
- Either best of worst case does not produce severe I/O amplification (it produce I/Os, but I/O rate is constant) 
  unlike LSM-tree DB (proposed design is similar to LSM-tree which has only level-0)
  

With NVM
--------
With NVM, we are able to make use of a different scheme to store the data depending replication
performance.
All of the data store NVM first with out-of-place update manner, then flush them to NVMe storage device
using in-place update scheme. This is feasible when replication latency (> 0.5 ms) is slower than
NVMe storage performance (< 0.5 ms) because flush can store all buffered data in NVM in batch-manner.


Detailed Design 
===============

WAL
---
WALmanager manages a WAL paritition in a sharded partition.

Partition
---------
Initially, PoseidonStore emploies static allocation of partition. The number of sharded partitions
is fixed and the size of each partition also should be configured before runing cluster.
PartitionManager maintain partition infos.

Disk layout
-----------

Cache
-----
There are two types of cache: 1) metadata, 2) data.


Discussion
==========


[1] Stathis Maneas, Kaveh Mahdaviani, Tim Emami, Bianca Schroeder:
A Study of SSD Reliability in Large Scale Enterprise Storage Deployments. FAST 2020: 137-149
