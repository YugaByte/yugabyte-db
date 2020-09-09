---
title: Transactional IO path
headerTitle: Transactional IO path
linkTitle: Transactional IO path
description: Learn how YugabyteDB manages the write path of a transaction.
aliases:
  - /architecture/transactions/transactional-io-path/
block_indexing: true
menu:
  stable:
    identifier: architecture-transactional-io-path
    parent: architecture-acid-transactions
    weight: 1156
isTocNested: true
showAsideToc: true
---

## Introduction

For an overview of some common concepts used in YugabyteDB's implementation of distributed
transactions, see [Distributed ACID transactions](../distributed-txns/) section. In this section, 
we will go over the write path of a transaction modifying multiple
keys, and the read path for reading a consistent combination of values from multiple tablets.

## Write path

Let us walk through the lifecycle of a single distributed write-only transaction. Suppose we are
trying to modify rows with keys `k1` and `k2`. If they belong to the same tablet, we could execute
this transaction as a [single-shard transaction](../../core-functions/write-path/), in which
case atomicity would be ensured by the fact that both updates would be replicated as part of the
same Raft log record. However, in the most general case, these keys would belong to different
tablets, and that is what we'll assume from this point on.

The diagram below shows the high-level steps of a distributed write-only transaction, not including
any conflict resolution.

![distributed_txn_write_path](/images/architecture/txn/distributed_txn_write_path.svg)

### 1. Client requests transaction

The client sends a request to a YugabyteDB tablet server that requires a distributed transaction. 
Here is an example using our extension to CQL:

 ```sql
 START TRANSACTION;
 UPDATE t1 SET v = 'v1' WHERE k = 'k1';
 UPDATE t2 SET v = 'v2' WHERE k = 'k2';
 COMMIT;
 ```

The tablet server that receives the transactional write request becomes responsible for driving all
the steps involved in this transaction, as described below. This orchestration of transaction steps
is performed by a component we call a *transaction manager*. Every transaction is handled by exactly
one transaction manager.

### 2. Create a transaction record

We assign a *transaction id* and select a *transaction status tablet* that would keep track of
a *transaction status record* that has the following fields:

- **Status** (pending, committed, or aborted)
- **Commit hybrid timestamp**, if committed
- **List of ids of participating tablets**, if committed

It makes sense to select a transaction status tablet in a way such that the transaction manager's tablet server is also the leader of its Raft group, because this allows to cut the RPC latency on querying and updating the transaction status. But in the most general case, the transaction status tablet might not be hosted on the same tablet server that initiates the transaction.

### 3. Write provisional records

We start writing *provisional records* to tablets containing the rows we need to modify.  These provisional records contain the transaction id, the values we are trying to write, and the provisional hybrid timestamp. This provisional hybrid timestamp is not the final commit timestamp, and will in general be different for different provisional records within the same transaction. In contrast, there is only one commit hybrid timestamp for the entire transaction.

As we write the provisional records, we might encounter conflicts with other transactions.  In
this case we would have to abort and restart the transaction. These restarts still happen
transparently to the client up to a certain number of retries.

### 4. Commit the transaction

When the transaction manager has written all the provisional records, it commits the transaction by sending an RPC request to the transaction status tablet. The commit operation will only succeed if the transaction has not yet been aborted due to conflicts.  The atomicity and durability of the commit operation is guaranteed by the transaction status tablet's Raft group. Once the commit operation is complete, all provisional records immediately become visible to clients.

The commit request the transaction manager sends to the status tablet includes the list of tablet IDs of all tablets that participate in the transaction. Clearly, no new tablets can be added to this set by this point. The status tablet needs this information to orchestrate cleaning up provisional records in participating tablets.

### 5. Send the response back to client

The YQL engine sends the response back to the client. If any client (either the same one or
different) sends a read request for the keys that were written, the new values are guaranteed to be reflected in the response, because the transaction is already committed. This property of a database is sometimes called the "read your own writes" guarantee.

### 6. Asynchronously applying and cleaning up provisional records

This step is coordinated by the transaction status tablet after it receives the commit message for our transaction and successfully replicates a change to the transaction's status in its Raft group. The transaction status tablet already knows what tablets are participating in this transaction, so it sends cleanup requests to them. Each participating tablet records a special "apply" record into its Raft log, containing the transaction id and commit timestamp. When this record is Raft-replicated in the participating tablet, the tablet remove the provisional records belonging to the transaction, and writes regular records with the correct commit timestamp to its RocksDB databases. These records now are virtually indistinguishable from those written by regular single-row operations.

Once all participating tablets have successfully processed these "apply" requests, the status tablet can delete the transaction status record, because all replicas of participating tablets that have not yet cleaned up provisional records (e.g. slow followers) will do so based on information available locally within those tablets. The deletion of the status record happens by writing a special "applied everywhere" entry to the Raft log of the status tablet. Raft log entries belonging to this transaction will be cleaned up from the status tablet's Raft log as part of regular garbage-collection of old Raft logs soon after this point.

## Read path

YugabyteDB is an [MVCC](https://en.wikipedia.org/wiki/Multiversion_concurrency_control) database, which means it internally keeps track of multiple versions of the same value. Read operations don't take any locks, and rely on the MVCC timestamp in order to read a consistent snapshot of the data. A long-running read operation, either single-shard or cross-shard, can proceed concurrently with write operations modifying the same key.

In the [ACID transactions](../) section, we talked about how up-to-date reads are performed from a single shard (tablet). In that case, the most recent value of a key is simply the value written by the last committed Raft log record that the Raft leader knows about. For reading multiple keys from different tablets, though, we have to make sure that the values we read come from a recent consistent snapshot of the database.  Here is a clarification of these two properties of the snapshot we have to choose:

- **Consistent snapshot**. The snapshot must show any transaction's records fully, or not show them at all. It cannot contain half of the values written by a transaction and omit the other half. We ensure the snapshot is **consistent** by performing all reads at a particular hybrid time (we will call it **ht_read**), and ignoring any records with higher hybrid time.

- **Recent snapshot**. The snapshot includes any value that any client might have already seen, which means all values that were written or read before this read operation was initiated.  This also includes all previously written values that **other components** of the client application might have written to or read from the database. The client performing the current read might rely on presence of those values in the result set, because those other components of the client application might have communicated this data to the current client through asynchronous communication channels.  We ensure the snapshot is **recent** by restarting the read operation when it is determined that the chosen hybrid time was too low, that is, there are some records that could have been written before the read operation was initiated but have a hybrid time higher than the currently chosen **ht_read**.

![Distributed transaction read path diagram](/images/architecture/txn/distributed_txn_read_path.svg)

### 1. Client's request handling and read transaction initialization

The client's request to either the YCQL or YEDIS or YSQL API arrives at the YQL engine of a tablet server. The YQL engine
detects that the query requests rows from multiple tablets and starts a read-only transaction.  A hybrid time **ht_read** is selected for the request, which could be either the current hybrid time on the YQL engine's tablet server, or the [safe time](../single-row-transactions/#safe-timestamp-assignment-for-a-read-request) on one of the involved tablets. The latter case would reduce waiting for safe time for at least that tablet and is therefore better for performance. Typically, due to our load-balancing policy, the YQL engine receiving the request will also host some of the tablets that the request is reading, allowing to implement the more performant second option without an additional RPC round-trip.

We also select a point in time we call **global_limit**, computed as `physical_time + max_clock_skew`, which allows us to determine whether a particular record was written *definitely after* our read request started. **max_clock_skew** is a globally configured bound on clock skew between different YugabyteDB servers. 

### 2. Read from all tablets at the chosen hybrid time

The YQL engine sends requests to all tablets the transaction needs to read from. Each tablet waits for **ht_read** to become a safe time to read at according to our [definition of safe time](../single-row-transactions/#definition-of-safe-time), and then starts executing its part of the read request from its local DocDB.

When a tablet server sees a relevant record with a hybrid time *ht_record*, it executes the
following logic:

- If **ht_record &le; ht_read**, include the record in the result.
- If **ht_record > definitely_future_ht**, exclude the record from the result.  The meaning of
   **definitely_future_ht** is that it it is a hybrid time such that a record with a higher hybrid
   time than that was definitely written *after* our read request started. You can assume
   **definitely_future_ht** above to simply be **global_limit** for now. We will clarify how exactly
   it is computed in a moment.
- If **ht_read < ht_record &le; definitely_future_ht**, we don't know if this record was written
   before or after our read request started. But we can't just omit it from the result, because if
   it was in fact written before the read request, this may produce a client-observed inconsistency.
   Therefore, we have to restart the entire read operation with an updated value of **ht_read =
   ht_record**.

To prevent an infinite loop of these read restarts, we also return a tablet-dependent hybrid time
value **local_limit<sub>tablet</sub>** to the YQL engine, computed as the current safe time in this
tablet. We now know that any record (regular or provisional) written to this tablet with a hybrid
time higher than **local_limit<sub>tablet</sub>** could not have possibly been written before our
read request started. Therefore, we won't have to restart the read transaction if we see a record
with a hybrid time higher than **local_limit<sub>tablet</sub>** in a later attempt to read from this
tablet within the same transaction, and we set **definitely_future_ht = min(global_limit,
local_limit<sub>tablet</sub>)** on future attempts.

### 3. Tablets query the transaction status

As each participating tablet reads from its local DocDB, it might encounter provisional records for
which it does not yet know the final transaction status and commit time. In these cases, it would
send a transaction status request to the transaction status tablet. If a transaction is committed,
it is treated as if DocDB already contained permanent records with hybrid time equal to the
transaction's commit time. The [cleanup](../transactional-io-path/#6-asynchronously-applying-and-cleaning-up-provisional-records)
of provisional records happens independently and asynchronously.

### 4. Tablets respond to YQL

Each tablet's response to YQL contains the following:

- Whether or not read restart is required.
- **local_limit<sub>tablet</sub>** to be used to restrict future read restarts caused by this
    tablet.
- The actual values that have been read from this tablet.

### 5. YQL sends the response to the client

As soon as all read operations from all participating tablets succeed and it has been determined
that there is no need to restart the read transaction, a response is sent to the client using the
appropriate wire protocol.

## See also

To review some common concepts relevant to YugabyteDB's implementation of distributed transactions, see [Distributed ACID transactions](../distributed-txns/) section.
