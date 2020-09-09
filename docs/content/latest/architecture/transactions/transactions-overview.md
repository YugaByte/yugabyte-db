---
title: Transactions overview
headerTitle: Transactions overview
linkTitle: Transactions overview
description: An overview of how transactions work in YugabyteDB.
menu:
  latest:
    identifier: architecture-transactions-overview
    parent: architecture-acid-transactions
    weight: 1151
isTocNested: true
showAsideToc: true
---

Transactions and strong consistency are a fundamental requirement for any RDBMS. DocDB has been designed for strong consistency. It supports fully distributed ACID transactions across rows, multiple tablets and multiple nodes at any scale. Transactions can span across tables in DocDB.

A transaction is a sequence of operations performed as a single logical unit of work. The intermediate states of the database as a result of applying the operations inside a transaction are not visible to other concurrent transactions, and if some failure occurs that prevents the transaction from completing, then none of the steps affect the database at all.

{{< note title="Note" >}}

All update operations inside DocDB are considered to be transactions. This includes the following:
* Operations that update just one row and those that update multiple rows that reside on different nodes.
* If `autocommit` mode is enabled, each statement is executed as one transaction.

{{</note >}}

## Time synchronization

A transaction in a YugabyteDB cluster may need to be update multiple rows that span across nodes in a cluster. In order to be ACID compliant, the various updates made by this transaction should be visible instantaneously as of a fixed time, irrespective of the node in the cluster that reads the update. In order to achieve this, it becomes essential for the nodes of the cluster to agree on a global notion of time.

Getting the different nodes of a cluster to agree on time requires all nodes to have access to a highly available and globally synchronized clock. [TrueTime](https://cloud.google.com/spanner/docs/true-time-external-consistency), used by Google Cloud Spanner, is an example of a highly available, globally synchronized clock with tight error bounds. However, such clocks are not available in many deployments. **Physical time clocks** (or wall clocks) cannot be perfectly synchronized across nodes. Hence they cannot order events (to establish a causal relationship) across nodes.

### Hybrid Logical Clocks

YugabyteDB uses **Hybrid Logical Clocks** or HLC’s solve the problem by combining physical time clocks that are coarsely synchronized using NTP with Lamport clocks that track causal relationships.

Each node in a YugabyteDB cluster first computes its HLC. HLC is represented as a (physical time component, logical component) tuple. HLCs generated on any node are strictly monotonic, and are compared as a tuple. When comparing two HLCs, the physical time component takes precedence over the logical component.

* **Physical time component:** YugabyteDB uses the physical clock (`CLOCK_REALTIME` in Linux) of a node to initialize the physical time component of its HLC. Once initialized, the physical time component can only get updated to a higher value. A monotonic clock is used since it represents the time elapsed since some arbitrary, fixed point in the past and guarantees that the time source is strictly linearly increasing.

* **Logical component:** For a given physical time component, the logical component of the HLC is a monotonically increasing number that provides ordering of events happening within that same physical time. This is initially set to 0. If the physical time component gets updated at any point, the logical component is reset to 0.

On any RPC communication between two nodes, HLC values are exchanged. The node with the lower HLC updates its HLC to the higher value. If the physical time on a node exceeds the physical time component of its HLC, the latter is updated to the physical time and the logical component is set to 0. Thus, HLC’s on a node are monotonically increasing.

{{< note title="Note" >}}

This same HLC is used to determine the read point in order to determine which updates should be visible to end clients. If an update has safely been replicated onto a majority of nodes as per the Raft protocol, that update operation can be acknowledged as successful to the client and it is safe to serve all reads up to that HLC. This forms the foundation for [lockless multi-version concurrency control (MVCC) in YugabyteDB](#mvcc).

{{</note >}}

## MVCC

YugabyteDB maintains data consistency internally using *multi-version concurrency control* (MVCC) without having to lock rows. Each transaction works on a version of the data in the database as of some hybrid timestamp. This prevents transactions from reading the intermediate updates made by concurrently running transactions, some of which may be updating the same rows. Each transaction, however, can see it's own updates, thereby providing transaction isolation for each database session. This technique of using MVCC minimizes lock contention when there are multiple concurrent transactions executing. 

### MVCC using hybrid time

YugabyteDB implements [multiversion concurrency control (MVCC)](https://en.wikipedia.org/wiki/Multiversion_concurrency_control) and internally keeps track of multiple versions of values corresponding to the same key, for example, of a particular column in a particular row. The details of how multiple versions of the same key are stored in each replica's DocDB are described in [Persistence on top of RocksDB](../../concepts/docdb/persistence). The last part of each key is a timestamp, which allows to quickly navigate to a particular version of a key in the RocksDB
key-value store.

The timestamp that we are using for MVCC comes from the [Hybrid Time](http://users.ece.utexas.edu/~garg/pdslab/david/hybrid-time-tech-report-01.pdf) algorithm, a distributed timestamp assignment algorithm that combines the advantages of local real-time (physical) clocks and Lamport clocks.  The Hybrid Time algorithm ensures that events connected by a causal chain of the form "A happens before B on the same server" or "A happens on one server, which then sends an RPC to another server, where B happens", always get assigned hybrid timestamps in an increasing order. This is achieved by propagating a hybrid timestamp with most RPC requests, and always updating the hybrid time on the receiving server to the highest value seen, including the current physical time on the server.  Multiple aspects of YugabyteDB's transaction model rely on these properties of Hybrid Time, e.g.:

* Hybrid timestamps assigned to committed Raft log entries in the same tablet always keep
  increasing, even if there are leader changes. This is because the new leader always has all
  committed entries from previous leaders, and it makes sure to update its hybrid clock with the
  timestamp of the last committed entry before appending new entries. This property simplifies the
  logic of selecting a safe hybrid time to pick for single-tablet read requests.

* A request trying to read data from a tablet at a particular hybrid time needs to make sure that no
  changes happen in the tablet with timestamps lower than the read timestamp, which could lead to an
  inconsistent result set. The need to read from a tablet at a particular timestamp arises during
  transactional reads across multiple tablets. This condition becomes easier to satisfy due to the
  fact that the read timestamp is chosen as the current hybrid time on the YB-TServer processing the
  read request, so hybrid time on the leader of the tablet we're reading from immediately gets
  updated to a value that is at least as high as the read timestamp.  Then the read request
  only has to wait for any relevant entries in the Raft queue with timestamps lower than the read
  timestamp to get replicated and applied to RocksDB, and it can proceed with processing the read
  request after that.


### Supported isolation levels

YugabyteDB supports two transaction isolation levels - `SNAPSHOT` (which is mapped to the SQL isolation level `REPEATABLE READ`) and `SERIALIZABLE` (which maps to the SQL isolation level of the same name). Read more about [isolation levels in YugabyteDB](../isolation-levels).

### Explicit locking

Just as with PostgreSQL, YugabyteDB provides various lock modes to control concurrent access to data in tables. These modes can be used for application-controlled locking in situations where MVCC does not give the desired behavior. Read more about [explicit locking in YugabyteDB](../explicit-locking).

{{< note title="Note" >}}

The architecture section covers the set of explicit locking modes currently supported by YugabyteDB. The plan is to cover most of the locking modes supported by PostgreSQL over time. 

{{</note >}}



## Transactions execution path

End user statements map to one of the following types of transactions inside YugabyteDB. The mapping of the user statements to transaction types is done seamlessly, the user does not need to be aware of the different types of transactions.


### Single-row transactions

The transaction manager of YugabyteDB automatically detects transactions that update a single row (as opposed to transactions that update rows across tablets or nodes). In order to achieve high performance, the updates to a single row directly update the row without having to interact with the transaction status tablet using the *single row transaction path*, also called the *fast path*. Read more about [the single row transactions IO path](../single-row-transactions).

{{< note title="Note" >}}

Because single row transactions do not have to update the transaction status table, they are much higher in performance than distributed transactions discussed in the next section. 

{{</note >}}


Below is list of single-row SQL statements that map to single row transactions.

#### `INSERT` statements

All single-row `INSERT` statements.
```
INSERT INTO table (columns) VALUES (values);
```

#### `UPDATE` statements

Single-row `UPDATE` statements that specify all the primary keys**

```
UPDATE table SET column = <new value> WHERE <all primary key values are specified>;
```

Single-row upsert statements using `UPDATE` .. `ON CONFLICT`. Note that the updates performed in case the row exists should match the set of values that were specified in the insert clause.

```
INSERT INTO table (columns) VALUES (values) 
    ON CONFLICT DO UPDATE
    SET <values>;
```


#### `DELETE` statements

Single-row `DELETE` statements that specify all the primary keys

```
DELETE FROM table WHERE <all primary key values are specified>;
```

### Distributed transactions

 A transaction that impacts a set of rows distributed across multiple tablets (which would be hosted on different nodes in the most general case) use the *distributed transactions path* to execute transactions. Implementing distributed transactions in YugabyteDB requires the use of a transaction manager that can coordinate the various operations that are a part of the transaction and finally commit or abort the transaction as needed. Read more about [the distributed transactions IO path](../transactional-io-path).

{{< note title="Note" >}}

YugabyteDB's distributed ACID transaction architecture is inspired by <a href="https://research.google.com/archive/spanner-osdi2012.pdf">Google Spanner</a>.

{{</note >}}


