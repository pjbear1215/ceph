===============
 PoseidonStore
===============

Key concepts and goals
======================

* As one of the pluggable backend stores for Crimson, PoseidonStore targets only 
  high-end NVMe SSDs (not conserned with ZNS devices).
* Hybrid update strategies for difference data types (in-place, out-of-place) to
  minimize CPU consumption by reducing host-side GC.
* Utilize NVMe feature (atomic large write command)
* Sharded data/processing model
* Support transactions

Background
----------

- Log-structured store 
  This kind of store manages a log to store/read the data. Whenever the write is coming 
  to the store, the data along with metadata is appended at the end of log. 
  - Pros
    - Without doubt, one sequential write is enough to store the data
    - It natually supports transaction (this is no overwrite, so the store can rollback 
      previous stable state)
    - Flash friendly (Definately, it mitigates GC burden on SSDs)
  - Cons
    - There is host-side GC that induces overheads 
      - I/O amplification (host-side)
      - More host-CPU consumption
    - Slow metadata lookup
    - Space overhead (live and unused data co-exist)

- In-place update store
  Conventional filsystems such as ext4, xfs have in-place update manner. This type of 
  the store overwrites data and metadta at fixed offset depending on a file.

  - Pros
    - No host-side GC is required
    - Fast lookup
    - No space overhead
  - Cons
    - More write occurs to record the data (metadata and data section are separated)
    - It cannot support transaction. WAL is required to support transactions
    - Give more burdens on SSDs due to device-level GC

Motivation
----------

In modern distributed storage system, a server node can be equipped with multiple 
NVMe storage devices. In fact, ten or more nodes could be attached on a server.
As a result, CPU resouces to acheive NVMe SSDs's performance is much higher than before
in terms of IOPS (I/O per seconds).
To fully exploit NVMe's performance, now we should consider host-side overhead that 
occurs due to high CPU consumption. In short, minimizing host-side CPU consumption
caused by storage processing is crucial.

From the perspective of CPU consumption in the store-level, the way that mitigate
resource usage is minimzing internel operation while processing I/O.
Conventional log-based store has a benefit of flash friendly data layout. 
However, Key-value DB based on LSM tree causes host-side I/O amplification 
including compaction. In addition, Log-structured store also produces unpredictable
I/O by garbage collection.

The ideal store would issues the same number of I/Os as requested by the user to the store 
device without I/O amplification. However, it is impossible to store the data via
only a single write because the store in Ceph requires supporting transaction which
requires ACID. Therefore, we intent to minimize I/O amplification while supporing
transaction by using hybrid update scheme depending data types.


Keyidea
-------

Out-of-place update scheme: | SB | Object metadata | Journal |
In-place update scheme: | SB | Object metadata | Allocation bitmap | Data blocks |

We are probably able to mitigate all the cons of the in-place update store 
combining out-of-place benefit as follows
- Use pre-allocation technique to reduce # of writes
- Ordered-mode-like with hybrid update scheme utilizing NVMe feature + help from 
  replica on recovery to minimize I/O amplification with supporting transaction
- High-end NVMe SSD has enuough powers to handle more works
- There is enough program-earse cycle limit [1]
- Depening on data type, some data does not induce much overheads caused by GC

Therefore, we propose PoseidonStore which makes use of different update strategy 
to minimize CPU consumption and I/O amplification

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
