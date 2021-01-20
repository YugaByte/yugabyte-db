Tracking GitHub Issue: https://github.com/yugabyte/yugabyte-db/issues/1004

# Automatic Re-sharding of Data with Tablet Splitting

Automatic tablet splitting enables changing the number of tablets (which are splits of data) at runtime. There are a number of scenarios where this is useful:

### 1. Range Scans
In use-cases that scan a range of data, the data is stored in the natural sort order (also known as *range-sharding*). In these usage patterns, it is often impossible to predict a good split boundary ahead of time. For example:

```
CREATE TABLE census_stats (
    age INTEGER,
    user_id INTEGER,
    ...
    );
```

In the table above, it is not possible for the database to infer the range of values for age (typically in the `1` to `100` range). It is also impossible to predict the distribution of rows in the table, meaning how many `user_id` rows will be inserted for each value of age to make an evenly distributed data split. This makes it hard to pick good split points ahead of time.

### 2. Low-cardinality primary keys
In use-cases with a low-cardinality of the primary keys (or the secondary index), hashing is not very effective. For example, if we had a table where the primary key (or index) was the column `gender` which has only two values `Male` and `Female`, hash sharding would not be very effective. However, it is still desirable to use the entire cluster of machines to maximize serving throughput.


### 3. Small tables that become very large
This feature is also useful for use-cases where tables begin small and thereby start with a few shards. If these tables grow very large, then nodes continuously get added to the cluster. We may reach a scenario where the number of nodes exceeds the number of tablets. Such cases require tablet splitting to effectively re-balance the cluster.


# Lifecycle of automatic re-sharding

There are four steps in the lifecycle of tablet splitting

1. **identifying tablets to split**
1. **initiating a split**
1. **performing the split**
1. **updating dependent components after split**

Each of these stages is described below.

## Identifying tablets to split

The YB-Master continuously monitors tablets and decides when to split a particular tablet. Currently, the data set size across the tablets is used to determine if any tablet needs to be split. This can be enhanced to take into account other factors such as:
* The data set size on each tablet (current)
* The IOPS on each tablet
* The CPU used on each tablet

Currently, the YB-Master configuration parameter `tablet_split_size_threshold_bytes` is propagated to all YB-TServers by piggybacking it with the heartbeat responses. The YB-TServers in turn report a list of tablets whose sizes exceed the `tablet_split_size_threshold_bytes` parameter.

Based on the heartbeats from all the YB-TServers, the YB-Master picks the set of tablets that need to be split. At this point, the split can be initiated.

## Initiating a split

The YB-Master registers two new post-split tablets and increments `SysTablesEntryPB.partitions_version` for the table.
Then, it sends a `SplitTablet()` RPC call to the appropriate YB-TServer with the pre-split tablet ID, post-split tablet IDs, and split key. Note that a server can split a tablet only if it hosts the leader tablet-peer.

Note: we use two new tablet IDs vs old tablet ID + one new tablet ID for the following reasons:
- The key range per tablet ID can be aggressively cached everywhere because it never changes.
- Handling of two new tablets' logs will be uniform vs. separate handling for the old tablet (but with reduced key
  range) and new tablet logs.

## Performing the split on YB-TServer side

When leader tablet server receives `SplitTablet` RPC, it adds a special Raft record containing:
- 2 new tablet IDs.
- Split key chosen as:
  - For range-based partitions: DocKey part of the approximate mid-key (we don’t want to split in the middle of the row).
  - For hash-based partitions: Hash code part of the approximate mid-key (hash-based partitions couldn’t be split in the middle of keys having the same hash code).
- We disallow processing any writes on the old tablet after split record is added to Raft log.

Tablet splitting happens at the moment of applying a Raft split-record and includes following steps:
- Do the RocksDB split - both regular and provisional records.
- Duplicate all Raft-related objects.
- Any new (received after split record is added) records/operations will have to be added directly to one of the two
new tablets.
- Before split record is applied old tablet is continuing processing read requests in a usual way.
- Old tablet will reject processing new read/write operations after split record apply is started. Higher layers will
have to handle this appropriately, update metadata and retry to new tablets.
- Once tablet splitting is complete on a leader of the original pre-split tablet - master will get info about new tablets in a
tablet report embedded into `TSHeartbeatRequestPB`. We can also send this info back as a response to
`TabletServerAdminService.SplitTablet` RPC so that master knows faster about new tablets.
- After leaders are elected for the new tablets - they are switched into `RUNNING` state.
- We keep the old tablet Raft group available but not serving reads/writes for the case when some old tablet replica hasn’t
received a Raft split record and hasn’t been split. For example, this replica was partitioned away before the split record
has been added into its Raft log and then it joins the cluster back after majority splits. There are the following cases:
  - This replica joins the cluster back in less than `log_min_seconds_to_retain` seconds: it will be able to get all Raft log
   records from the old tablet leader, split the old tablet on a replica, and then get all Raft log records for post-split
   tablets.
  - This replica joins the cluster back in less than `follower_unavailable_considered_failed_sec` but after
  `log_min_seconds_to_retain seconds`: part of the Raft log records absent on this replica would not be available on the old
  tablet leader, so remote bootstrap will be initiated.

    Note: we have logic to prevent a Raft split record from being GCed.
  - This replica joins the cluster back after follower_unavailable_considered_failed_sec. In this case replica is considered as failed and is evicted from the Raft group, so we don’t need to hold the old tablet anymore.

  Note: by default `follower_unavailable_considered_failed_sec` = `log_min_seconds_to_retain`, but these flags can be adjusted.


### Document Storage Layer splitting
- We copy the RocksDB to additional directory using hard links and add metadata saying that only
part of the key range is visible. Remote bootstrap will work right away.
Next major compaction will remove all key-value pairs which are no longer related to the new tablets due to
split. Later, we can implement cutting of RocksDB without full compaction (see below in this section).
- We store tablet's key bounds inside `KvStoreInfo` tablet metadata. `BoundedRocksDbIterator` filters out non relevant keys. We can
also propagate key boundaries to a regular RocksDB instance so that it has knowledge about non relevant keys, and we can
implement RocksDB optimizations like truncating SST files quickly even before the full compactions.
- **Performance note: remote bootstrap could download irrelevant data from new tablet if remote bootstrap is happening
before compaction.**

- Snapshots (created by `Tablet::CreateSnapshot`)
  - The simplest solution is to clone snapshot as well using hard-link, but snapshots are immutable and once they will
  be re-distributed across different tservers it will increase space amplification. Snapshots are currently only used
  for backup and are quickly deleted. But it would be good to implement cutting of RocksDB without full compaction -
  could be done later.
- Cutting of RocksDB without full compaction
  - We can “truncate” SST S-block and update SST metadata in case we cut off the first half of S-block, so RocksDB
  positions correctly inside S-block without need to update data index.

### Provisional records DB splitting
We have the following distinct types of data in provisional records (intents) store:
- Main provisional record data:
`SubDocKey` (no HybridTime) + `IntentType` + `HybridTime` -> `TxnId` + value of the provisional record
- Transaction metadata:
`TxnId` -> status tablet id + isolation level
- Reverse index by Txn ID:
`TxnId` + `HybridTime` -> Main provisional record data key

`TxnId`, `IntentType`, `HybridTime` are all prefixed with appropriate value type.

Reverse index by transaction ID is used for getting all provisional records in tablet related to particular transaction.
We can’t just split provisional records DB at RocksDB level by some mid-key because metadata is not sorted by original
key. Instead we can just duplicate provisional records DB and do filtering by original key inside
`docdb::PrepareApplyIntentsBatch` which is the only function using provisional records reverse index.
Filtering of main provisional record data will be done inside `BoundedRocksDbIterator`.

## Updating dependent components after split

`SysTablesEntryPB.partitions_version` is included into `GetTableLocationsResponsePB`.

### YBClient
- `YBTable::partitions_` is first populated inside `YBTable::Open`. We refresh it on the following events:
  - `MetaCache::ProcessTabletLocations` gets a newer partition_version from `GetTableLocationsResponsePB`.
  - Request to a tablet leader gets rejected because the tablet has been split. As an optimization, we can include necessary
  information about new tablets into this response for YBClient to be able to retry on new tablets without reaching to
  master.
- Each time `YBTable::partitions_` is updated we also update the meta cache.

### Distributed transactions
- Until old tablet is deleted, it receives “apply” requests from TransactionManager and will reject them. As a part of
reject response it will send back new tablets IDs, so TransactionManager will retry “apply” requests to new tablets.
- When old tablet is deleted, it also checks its provisional records DB for transactions in which the tablet participates
and sends them info to update its tablet ID to new tablets IDs.

## Other components
We should disallow tablet splitting when the split is requested from a tablet leader that is remote-bootstrapping one
of the nodes.
Bulk load tool relies on partitioning to be fixed during the load process, so we decided to pre-split and disable
dynamic splitting during bulk load.

[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/design/docdb-automatic-tablet-splitting.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
