# Two Data Center Deployment Support with YugabyteDB

This document outlines the 2-datacenter deployments that YugabyteDB is being enhanced to support, as well as the architecture that enables these. This feature is built on top of [Change Data Capture (CDC)](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-change-data-capture.md) support in DocDB, and that design may be of interest.

### Watch an example video (less than 2 mins)
<a href="http://www.youtube.com/watch?feature=player_embedded&v=2quaIAKBATk" target="_blank">
  <img src="http://img.youtube.com/vi/2quaIAKBATk/0.jpg" alt="YugabyteDB 2DC deployment" width="240" height="180" border="10" />
</a>

## Features

> **Note:** In this design document, the terms "cluster" and "universe" will be used interchangeably. While not a requirement in the final design, we assume here that each YugabyteDB universe is deployed in a single data-center for simplicity purposes.

This feature will support the following:

* Replication across datacenters is done at the DocDB level. This means that replication will work across **all** the APIs - YSQL, YCQL and YEDIS.

* The design is general enough to support replication of single-key updates as well as multi-key/distributed transactions.

* In the initial version, we assume that schema changes are run on both the universes independently. This will eventually be automated to the extent possible. Note that some combinations of schema changes and update operations are inherently unsafe. Identifying all of these cases and making the database safe in all scenarios (by throwing a user facing error out or through some other such mechanism) will be a follow on task to harden this feature.

* Design will support active-active replication, with both data centers accepting writes and replicating them to the other data center.

* Note that it will be possible to perform replication to multiple target clusters. Similarly, it will be possible to consume replicated data from multiple source clusters.

* Updates will be timeline consistent. That is, target data center will receive updates for a row in the same order in which they occurred on the source.

* Transactions will be applied atomically on the consumer. That is, either all changes in a transaction should be visible or none.

* Target data center will know the data consistency timestamp. Since YB data is distributed across multiple nodes, (and data is replicated from multiple nodes), target data center should be able to tell that all tablets have received data at least until timestamp x, that is, it has received all the writes that happened at source data center(s) until timestamp x.


# Supported Deployment Scenarios

The following 2-DC scenarios will be supported:

### Master-Slave with *asynchronous replication*

The replication could be unidirectional from a **source cluster** (aka the *master* cluster) to one or more **sink clusters** (aka *slave* clusters). The sink clusters, typically located in data centers or regions that are different from the source cluster, are *passive* because they do not not take writes from the higher layer services. Such deployments are typically used for serving low latency reads from the slave clusters as well as for disaster recovery purposes.

The source-sink deployment architecture is shown in the diagram below:

![2DC source-sink deployment](https://github.com/yugabyte/yugabyte-db/raw/master/architecture/design/images/2DC-source-sink-deployment.png)

### Multi-Master with *last writer wins (LWW)* semantics

The replication of data can be bi-directional between two clusters. In this case, both clusters can perform reads and writes. Writes to any cluster is asynchronously replicated to the other cluster with a timestamp for the update. If the same key is updated in both clusters at a similar time window, this will result in the write with the larger timestamp becoming the latest write. Thus, in this case, the clusters are all *active* and this deployment mode is called **multi-master deployments** or **active-active deployments**).

> **NOTE:** The multi-master deployment is built internally using two master-slave unidirectional replication streams as a building block. Special care is taken to ensure that the timestamps are assigned to ensure last writer wins semantics and the data arriving from the replication stream is not re-replicated. Since master-slave is the core building block, we will focus the rest of this design document on that.


The architecture diagram is shown below:

![2DC multi-master deployment](https://github.com/yugabyte/yugabyte-db/raw/master/architecture/design/images/2DC-multi-master-deployment.png)



# Design - Lifecycle of Replication

2DC one-way sync replication in YugabyteDB can be broken down into the following phases:

* Initialize the producer and the consumer
* Set up a distributed CDC
* Pull changes from source cluster
* Periodically checkpoint progress

Each of these are described below.

### Step 1. Initialize the producer and the consumer

In order to setup 2DC async replication, the user runs a command similar to the one shown below:

```
yugabyte-db init-replication                         \
            --sink_universe <sink_ip_addresses>      \
            --source_universe <source_ip_addresses>  \
            --source_role <role>                     \
            --source_password <password>
```

> **NOTE:** The commands shown above are for illustration purposes, these are not the final commands. Please refer to the user documentation of this feature after it has landed for the final commands.

This instructs the sink cluster to perform the following two actions:
* Initialize its **sink replication consumer** which prepares it to consume updates from the source.
* Make an RPC call to the source universe to initialize the **source replication producer**, which prepares it to produce a stream of updates.

This is shown diagrammatically below.

![2DC initialize consumer and producer](https://github.com/yugabyte/yugabyte-db/raw/master/architecture/design/images/2DC-step1-initialize.png)

The **sink replication consumer** and the **source replication producer** are distributed, scale-out services that run on each node of the sink and source clusters respectively. In the case of the *source replication producer*, each node is responsible for all the source tablet leaders it hosts (note that the tablets are restricted to only those that participate in replication). In the case of the *sink replication consumer*, the metadata about which sink node owns which source tablet is explicitly tracked in the system catalog.

Note that the sink clusters can fall behind the source clusters due to various failure scenarios (such as node outages or extended network partitions). Upon the failure conditions healing, it may not always be safe to continue replication. In such cases, the user would have to bootstrap the sink cluster to a point from which replication can safely resume. Some of the cases that arise are shown diagrammatically below:
```
                          ╔══════════════════════════════════╗
                          ║      New replication setup?      ║
                          ╚══════════════════════════════════╝
                                    |               |
  ╔══════════════════════╗      YES |               | NO       ╔══════════════════════╗
  ║   Does data already  ║<----------               ---------->║ Can CDC continue     ║
  ║   exist in cluster?  ║                                     ║ from last checkpoint?║
  ╚══════════════════════╝                                     ╚══════════════════════╝
         |          |________________                _______________|        |
      YES|                 NO        |              |      YES               |NO
         V                           V              V                        V
  ╔══════════════════════╗        ╔══════════════════════╗      ╔══════════════════════╗
  ║ First bootstrap data ║        ║   Replication can    ║      ║ Re-bootstrap data    ║
  ║ from source cluster  ║------->║   resume safely      ║<-----║ from source cluster  ║
  ╚══════════════════════╝  DONE  ╚══════════════════════╝ DONE ╚══════════════════════╝
```

### Step 2. Set up a distributed CDC

The next step is to start the change data capture processes on the source cluster so that it can begin generating a list of changes. Since this feature builds on top of *Change Data Capture (CDC)*, the change stream has the following guarantees:

* **In order delivery per row**: All changes for a single row (or rows in the same tablet) will be received in the order in which they happened.
  > Note that global ordering is not guaranteed across tablets.

* **At least once delivery**: Updates for rows will be pushed at least once. It is possible to receive previously seen updates again.

* **Monotonicity of updates**: Receiving a change for a row implies that all previous changes for that row have been received.

The following substeps happen as a part of this step:

* The sink cluster fetches the list of source tablets.
* The responsibility of replication from a subset of source tablets is assigned to each of the nodes in the sink cluster.
* Each node in the sync cluster pulls the change stream from the source cluster for the tablets that have been assigned to it. These changes are then applied to the sink cluster.

The sync cluster maintains the state of replication in a separate `system.cdc_state` table. The following information is tracked for each tablet in the source cluster that needs to be replicated:
* Details of the source tablet, along with which table it belongs to
* The node in the sink cluster that is currently responsible for replicating the data
* The last operation id that was successfully committed into the sink cluster

### Step 3. Fetch and handle changes from source cluster

The *sink replication consumer* continuously performs a `GetCDCRecords()` RPC call to fetch updates (which returns a `GetCDCRecordsResponsePB`). The source cluster sends a response that includes a list of update messages that need to be replicated, if any. It may optionally also include a list of IP addresses of new nodes in the source cluster in case tablets have moved. An example of the response message is shown below.

```
GetCDCRecordsResponsePB Format:
╔══════════╦══════════╦══════════════════╦═══════════╦═════════════════════════════════════════════╗
║  Source  ║  Source  ║ CDC type: CHANGE ║  Source   ║ <---------- ReplicateMsg list ----------->  ║
║ universe ║  tablet  ║ (send the final  ║  server   ║ ╔═══════════════════════════════╦═════════╗ ║
║   uuid   ║   uuid   ║  changed values) ║ host:port ║ ║ document_key_1, update msg #1 ║   ...   ║ ║
║          ║          ║                  ║           ║ ╚═══════════════════════════════╩═════════╝ ║
╚══════════╩══════════╩══════════════════╩═══════════╩═════════════════════════════════════════════╝
```

As shown in the figure above, each update message has a corresponding **document key** (denoted by the `document_key_1` field in the figure above) which the *sink replication consumer* uses to locate the owning tablet leader on the sink cluster. The update message is routed to that tablet leader, which handles the update request. See the section below on transactional guarantees for details on how this update is handled.

> Note that the `GetCDCRecords()` RPC request can be routed to the appropriate node through an external load balancer in the case of a Kubernetes deployment.

### Step 4. Periodically checkpoint progress

The sink cluster nodes will periodically write a *checkpoint* consisting of the last successfully applied operation id along with the source tablet information into its `system.cdc_state` table. This allows the replication to continue from this point onwards.

# Transactional Guarantees

Note that the CDC change stream generates updates (including provisional writes) at the granularity of document attributes and not at the granularity of user-issued transactions. However, transactional guarantees are important for 2DC deployments. The *sink replication consumer* ensures the following:

* **Atomicity of transactions**: This implies one can never read a partial result of a transaction on the sink cluster.
* **Not globally ordered**: The transactions (especially those that do not involve overlapping rows) may not be applied in the same order as they occur in the source cluster.
* **Last writer wins**: In case of active-active configurations, if there are conflicting writes to the same key, then the update with the larger timestamp is considered the latest update. Thus, the deployment is eventually consistent across the two data centers.

The following sections outline how these transactional guarantees are achieved by looking at how a tablet leader handles a single `ReplicateMsg`, which has changes for a document key that it owns.


### Single-row transactions

```
ReplicateMsg received by the tablet leader of the sink cluster:
╔══════════════╦═══════════╦════════════════════╦════════════════════════╗
║ document_key ║ new value ║   OperationType:   ║  TransactionMetadata:  ║
║              ║           ║       WRITE        ║         (none)         ║
╚══════════════╩═══════════╩════════════════════╩════════════════════════╝
```

In the case of single-row transactions, the `TransactionMetadata` field would not have a `txn uuid` set since there are no provisional writes. Note that in this case, the only valid value for `OperationType` is `write`.

On receiving a new single-row write `ReplicateMsg` for the key `document_key`, the tablet leader owning that key processes the `ReplicateMsg` according to the following rules:
* **Ignore on later update**: If there is a committed write at a higher timestamp, then the replicated write update is ignored. This is safe because a more recent update already exists, which should be honored as the latest value according to a *last writer wins* policy based on the record timestamp.
* **Abort conflicting transaction**: If there is a conflicting non-replicated transaction in progress, that transaction is aborted and the replicated message is applied.
* **In all other cases, the update is applied.**

Once the `ReplicateMsg` is processed by the tablet leader, the *sink replication consumer* marks the record as `consumed`

> Note that all replicated records will be applied at the same hybrid timestamp at which they occurred on the producer universe.


### Multi-row transactions

```
ReplicateMsg received by the tablet leader of the sink cluster:
╔══════════════╦═══════════╦════════════════════╦═══════════════════════╗
║              ║           ║   OperationType:   ║  TransactionMetadata: ║
║ document_key ║ new value ║       WRITE        ║    - txn uuid         ║
║              ║           ║         or         ║    - hybrid timestamp ║
║              ║           ║  CREATE/UPDATE TXN ║    - isolation level  ║
╚══════════════╩═══════════╩════════════════════╩═══════════════════════╝
```

In the case of single-row transactions, the `TransactionMetadata` field be set. Note that in this case, the `OperationType` can be `WRITE` or `CREATE/UPDATE TXN`.

Upon receiving a `ReplicateMsg` that is a part of a multi-row transaction, the *sink replication consumer* handles it as follows:

* `CREATE TXN` messages: Send the message to the transaction status tablet leader (based on the value of `txn uuid` in the `TransactionMetadata` field. The message eventually gets applied to the transaction status table on the sink cluster.
* `WRITE` messages: Sends the record to tablet leader of the key, which ensures there is no conflict before writing a provisional write record.
    * If there is a conflicting provisional write as a part of a non-replicated transaction on the sink cluster, then this transaction is aborted.
    * If there is another non-transactional write recorded at a higher timestamp, then the leader does not write the record into its WAL log but returns success to replication leader (The write is ignored but considered “success”).

* `UPDATE TXN` messages: The following cases are possible:
    * If record is a `Transaction Applying` record for a tablet involved in the transaction, then an RPC is sent to the transaction leader, indicating that the tablet involved has received all records for the transaction. The transaction leader will record this information in its WAL and transactions table (using field tablets_received_applying_record).
    * If record is `Transaction Commit` record, then all tablets involved in the transaction are queried to see if they have received their `Transaction Applying` records.
        * If not, then simply add a `WAITING_TO_COMMIT` message to its WAL and set the transaction status to `WAITING_TO_COMMIT`.
        * If all involved tablets have received `Transaction Applying` message, then set the status to `COMMITTED`, and sends a new `Transaction Applying` wal message to all involved tablets, notifying them to commit the transaction.
    * In applying record above, if the status is `WAITING_TO_COMMIT` and all involved tablets have received their applying message, then it applies a new `COMMITTED` message to WAL, changes transaction status to `COMMITTED` and sends a new applying message for all involved tablets, notifying them to commit the transaction.

This combination of `WAITING_TO_COMMIT` status and `tablets_received_applying_record` field will be used to ensure that transaction records are applied atomically on the replicated universe.

# Handling network partitions and consumer failures

As mentioned before, we replicate records from the WAL (internally known as log) only, so if WAL files containing unreplicated records are garbage collected because all of its records have already been flushed to RocksDB, then we won't be able to replicate these records. So when a network partition happens between two universes being replicated (2DC), or when the consumer data center fails, we want to keep the logs in the producer as long as possible so that once the network or the consumer data center recovers, we can continue replicating the changes to the consumer using the normal replication mechanism (no bulk copy of data required).

## Current log retention policies

Currently, we have two different policies for deciding which log segments to retain: a time-based policy and a policy based on the minimum number of files. These policies are controlled by the gflags `log_min_seconds_to_retain` with a default value of `300` and `log_min_segments_to_retain` with a default value of `2`.

For the time-based policy, the Log object also uses a private variable `wal_retention_secs_` which gets modified through an `ALTER TABLE` request. When deciding if a log file is old enough to be deleted, we use `max(log_min_seconds_to_retain, wal_retention_secs_)`.

To avoid deleting log segments that contain unreplicated entries, we will add a new policy based on the minimum op id that we want to retain. This new policy will be disabled by default and will be controlled by the gflag `enable_log_retention_by_op_idx`.

Because of the possibility of very long network partitions or unused cdc streams that were not deleted, we will have an option in which the op id policy is ignored if the tserver is running out of disk space. If the gflag `log_stop_retaining_min_disk_mb` is set, we will stop retaining records with unreplicated entries once disk space is less than the specified value. This gflag will not affect log segments whose entries haven’t been flushed to RocksDB.

We will also provide a new gflag `log_max_seconds_to_retain` to specify the maximum retention time. If a log file is older than this gflag, we will delete it even if one if its records hasn’t been replicated, but only if deleting such file doesn’t violate any of the other policies specified by `log_min_seconds_to_retain` and `log_min_segments_to_retain`. If log_max_seconds_to_retain is set to a value less than or equal to `max(log_min_seconds_to_retain, wal_retention_secs_)`, it will be ignored.

## Last replicated op id
Our 2DC implementation can only replicate entries that are in the log files (it’s not possible to replicate entries that are only in RocksDB), so it’s important that we don’t delete log files that contain unreplicated entries.

When a consumer polls for changes, it sends to the producer the latest replicated op id, and this information is saved on the producer's `cdc_state` table. The producer will take this information and call `Log::SetMinimumOpIdToRetain()`. This will in turn set the private variable `minimum_op_idx_to_retain_` which will be used by the Log garbage collector only when enable_log_retention_by_op_idx is set to true.

`Log::SetMinimumOpIdToRetain()` will be called every time a cdc stream’s last replicated op id is updated (in `CDCServiceImpl::UpdateCheckpoint`) and every time the cdc service starts.

## Setting up the op id retention policy
For this policy to be active, all the tservers in the producer universe will have to be started with `--enable_log_retention_by_op_idx=true`. Initially, if no cdc stream is setup for a specific table, the policy will remain disabled for all the tablets in the table. Once the first cdc stream is setup, `Log::SetMinimumOpIdToRetain()` will be called with a value of 0. This will effectively disable garbage collection of log files until the consumer updates the latest replicated op id.

Since we don’t have a mechanism to bootstrap a consumer once necessary log files have been deleted, this design only works when a 2DC consumer is setup soon after a table has been created in the producer. By default, `log_min_seconds_to_retain` is `300`, but we will allow users to modify this value for a specific table through the `yb-admin` command. That way, only log files for a specific table are retained for a long time so that when a cdc stream is created, all the records are available in the log files.

## Updating the last replicated op id
Because more than one CDC stream could exist for the same tablet, we will need to use the minimum replicated op id for that tablet. This information is saved in the cdc_state table. A new method called `CDCServiceImpl::GetMinAppliedCheckpointForTablet` will read the minimum applied index for all streams of a specific tablet. This include stale streams since that's the problem we are trying to solve with these new policies.

## Propagating last replicated op id to followers
A new background thread  gets started once `CDCServiceImpl` is instantiated. Inside this thread, the method `CDCServiceImpl::ReadCdcMinReplicatedIndexForAllTabletsAndUpdatePeers` runs periodically (controlled by the gflag `update_min_cdc_indices_interval_secs`). Each time it runs, it reads the `cdc_state` table to obtain the minimum applied index for each tablet for which one of the local tablet peers is a leader. After obtaining this information, it broadcasts this information to the tablet followers. This update happens through the RPC call `UpdateCdcReplicatedIndexRequest`. This is a best-effort propagation since it is safe if the followers miss this information since at worst, they will have an older op id.

Potential future improvements:
1) If we want to guarantee that all the followers get the last replicated op id, we could create a new master RPC similar to `ALTER TABLE`, and the master would be in charge of guaranteeing that all the followers eventually get the latest information. This might not provide any benefit because we would have to limit how often we call this RPC to avoid flooding the master leader with a request each time the consumer updates the replicated op id.

2) Just as we save wal_retention_secs in the tablet metadata, we could do the same for `minimum_op_idx_to_retain_` so that whenever a new tablet is bootstrapped, it receives this information before it begins its local bootstrap process. This information would also survive  tserver restarts. Again, this might not provide any benefit, since whenever a tserver is remote bootstrapped, it could assume that it has to retain all the logs received until it gets the correct op id through the heartbeat.

## Deleting a CDC stream
Currently we don’t delete entries from cdc_state when a cdc stream is deleted. In order to implement the op id retention policy, we will need to fix this issue. Otherwise, we will fill disks with unnecessary log files because our minimum replicated op id for a specific tablet will not change. We will need to eventually make this operation fault tolerant so we don’t end retaining logs unnecessarily. In the meantime, we will mitigate this issue by using the flags `log_max_seconds_to_retain` and `log_stop_retaining_min_disk_mb`, and by providing a way to manually delete entries from cdc_state.

Once the last cdc stream for a tablet is deleted from cdc_state, we will call `Log::SetMinimumOpIdToRetain()` and set it to `std::numeric_limits<uint64_t>::max`.


# What’s supported in the initial version?
* **Active-active (multi-master) replication**:
  * If there is a write conflict (i.e. same row is updated on both universes), then we’ll follow the “last writer wins” semantics and the write with the latest timestamp will eventually persist on both data centers.

* **Replicating newly added tables**:
  * Support setting up replication on new tables that are added after 2DC is setup. In this situation, users will need to run yb-admin command and list the new table to consume from.

* **Setting up replication on empty tables**:
  * Setup replication when the table has no data. We cannot handle setting up replication on a table with existing data.


# Future Work

* **Automatically bootstrapping sink clusters**:
  * Currently: it is the responsibility of the end user to ensure that a sink cluster has sufficiently recent updates so that replication can safely resume.
  * Future: bootstrapping the sink cluster can be automated.

* **Replicating DDL changes**:
  * Currently: DDL changes are not automatically replicated. Applying create table and alter table commands to the sync clusters is the responsibility of the user.
  * Future: Allow safe DDL changes to be propagated automatically.

* **Safety of DDL and DML in active-active**:
  * Currently: Certain potentially unsafe combinations of DDL/DML are allowed. For example, in having a *unique key constraint* on a column in an active-active *last writer wins* mode is unsafe since a violation could easily be introduced by inserting different values on the two clusters - each of these operations is legal in itself. The ensuing replication can, however, violate the unique key constraint. This will cause the two clusters to permanently diverge and the replication to fail.
  * Future: Detect such unsafe combinations and warn the user. Such combinations should possibly be disallowed by default.

# Impact on Application Design
Since 2DC replication is done asynchronously and by replicating the WAL (and thereby bypassing the query layer), application design needs to follow these patterns:

* **Avoid UNIQUE indexes / constraints (only for active-active mode)**:
  * Since replication is done at the WAL level, we don’t have a way to check for unique constraints. It’s possible to have two conflicting writes on separate universes which will violate the unique constraint and will cause the main table to contain both rows but the index to contain just 1 row, resulting in an inconsistent state.

* **Avoid triggers**:
  * Since we bypass the query layer for replicated records, DB triggers will not be fired for those and can result in unexpected behavior.

* **Avoid serial columns in primary key (only for active-active mode)**:
  * Since both universes will generate the same sequence numbers, this can result in conflicting rows. It’s better to use UUIDs  instead.


[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/design/multi-region-2DC-deployment.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
