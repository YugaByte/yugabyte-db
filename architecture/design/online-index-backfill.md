# Online Index Backfill

This design document explains how online backfill of indexes in YugabyteDB works. Upon adding new indexes to a table that already has data, this feature would enable building these indexes in an online manner, while continuing to serve other traffic. Note that this feature should work across both YSQL and YCQL APIs.

## Design Goals

* **Online builds:** Support building the indexes without locking out reads or writes on the table. The index build itself will occur asynchronously.
* **Correctness:** After the index builds are completed, they should be consistent with the data in the primary table.
* **Constraint violations:** If a problem arises while scanning the table, such as a unique constraint violation in a unique index, the `CREATE INDEX` command should abort and result in a failure. An aborted index will be cleaned up and deleted. Details (such as which constraints were violated) will be found in the logs.
* **Efficient for large datasets:** Index build should occur in a distributed manner (utilizing multiple/all nodes in the cluster) to efficiently handle large datasets.
* **Resilience:** The index build should be resilient to failures. The entire build process should not need to restart on a node failure in the cluster.

> **Note:** Online index backfill relies on the online schema change framework. This design doc assumes the reader is familiar with how online schema changes are handled in YugabyteDB.

# Design

Before a schema change is initiated, the currently active copy of the schema is stored in the YB-Master, and cached on all the nodes (YB-TServers). The process of schema change is initiated by sending an RPC call to the YB-Master. The overall protocol for safely creating and backfilling an index (in other words, the protocol for performing online schema changes) relies on transitioning through various intermediate states explained below.

Let us say that we have a table `MyTable` with pre-existing data and we are adding an index `MyIndex` to this table.

## Intermediate states of the index table

Once the updates are made, the YB-Master leader then creates the desired number of new tablets for the index table `MyIndex` and sends asynchronous `HandleAlterTable()` requests to each tablet leader of the table. Typically, until the backfill process is complete, the newly created index will *not* be available for any reads. However, incoming write operations that are concurrent with the backfill process may need to update the index.

The backfill process moves through the following 4 states (after the `MyIndex` index table has already been created). The currently active state of the `MyIndex` index is persisted by the YB-Master and replicated across all the YB-TServers as a part of the system catalog / metadata. The `IndexPermissions` state entry for `MyIndex` is used to determine what kind of index updates/access will be allowed against the index at any point in time.

* **`DELETE_ONLY`:** In this state, whenever a row in `MyTable` is updated, the delete operation on the index (corresponding to the old value) is applied to the `MyIndex` index table. However, writes to the index (corresponding to the new value) are prohibited. All the queries/updates continue against `MyTable` (and the existing indexes if any).

    For example, in a typical update operation consisting of the following steps, only the `DELETE` operation is applied to the index:
    ```
    BEGIN INTERNAL TRANSACTION
        UPDATE the primary MyTable table
        DELETE the old value in the MyIndex index table
        INSERT the new value into the MyIndex index table
    COMMIT INTERNAL TRANSACTION
    ```

* **`WRITE_AND_DELETE`:** In this state, whenever a row is updated, the the following operations are applied to the `MyIndex` index table:
    - delete the old value
    - update the new value

    The update to the index is performed using the *current* hybrid timestamp, which is the same hybrid time used to insert the values into the primary table. This ensures that once the update completes, the hybrid timestamp of the entry in primary table and the index table are the same. The index can still not be used to perform read operations.


* **`DB_REORG`:** This is the state where the YB-TServers actually perform the backfill. This state could take a long time to complete, this depends on the dataset size. In terms of operations applied to the `MyIndex` table, this state is similar to `WRITE_AND_DELETE` - all inserts, updates and deletes are applied to the `MyIndex` index table. The index cannot be used to perform read operations, which still are satisfied by the primary table `MyTable` (and any existing indexes if any).

    > **Note:** The details of the backfill process are covered in a dedicated section below.

* **`READ_WRITE_AND_DELETE`:** This is the final state, where the `MyIndex` index can be used to serve reads.


## The index backfill algorithm

The steps below outline the index backfill algorithm.

### 1. Handle `CREATE INDEX` statement at YB-TServer

The `CREATE INDEX` statement (approximate syntax shown below) can be sent to any node of the cluster. Note that the exact details of this statement depends on the exact API being used (YSQL or YCQL).
```
CREATE INDEX MyIndex on MyTable (...);
```

This statement is parsed and executed by the YB-TServer, which results in an `AlterTable()` RPC call to the YB-Master leader. This RPC call which kicks off the multi-stage online schema change, of which the the index build is one stage. The `CREATE INDEX` command is asynchronous, it does not wait for the index backfill to complete. It would be possible to query and determine the status (`IN-PROGRESS`, `SUCCESS`, `FAILED`) of the asynchronous job.

### 2. Create the index table

Upon receiving the `AlterTable()` RPC call, the YB-Master first performs the requisite updates to the system catalog / metadata in a transactional manner. The updates essentially do the following:

* Adds a reference from the list of indexes in the primary table `MyTable` to the new index `MyIndex`
* Stores the information about the index `MyIndex`
* Creates an entry in the `IndexPermissions` table for this `MyIndex` index, which determines what can be done with the index.
* Sets the state of the `MyIndex` index to `DELETE_ONLY`

> **Note:** The exact set of updates to the system catalog vary based on the API, meaning the set of updates performed in the case of YSQL would differ from YCQL since the metadata organization is different between the two APIs.

After setting the `MyIndex` index state to the `DELETE_ONLY`, the YB-Master leader sends the `HandleAlterTable()` RPC calls to the various YB-TServers. The `HandleAlterTable()` RPC call initiates a schema change on all the nodes in the cluster. Once the `HandleAlterTable()` call completes on all the tablets of the table, the YB-Master performs checks to see if another schema change is required (for example, in the case of adding multiple indexes to a table and building all of them at the same time). If another change is required, this results in another round of schema changes across all the tablets.

Once all the schema changes are propagated to all the nodes, the index state is updated from `DELETE_ONLY` to `WRITE_AND_DELETE`. Once all the schema changes converge, the index state finally gets set to `DB_REORG`.


### 3. Backfill the data

After the index state is updated to `DB_REORG`, the YB-Master orchestrates the backfill process by issuing `BackfillIndex()` RPC calls to each tablet. This starts building the index across all the tablets of the table `MyTable`. The YB-Master keeps track of how many tablets have completed the build. At this point, the YB-Master  needs to wait for the backfill to complete on all the tablets before updating the table to the `READ_WRITE_AND_DELETE` state.

> **Note:** Details of how the index backfill works on any tablet is covered in detail in the next section.


### 4. Finalize the index

Once the index build is successfully completed on all the tablets of the table, the table state is updated to `READ_WRITE_AND_DELETE`, at which point the index is completely rebuilt.


## The index backfill process

The backfill process is a background job that runs on each of the tablets of the `MyTable` table. Since each row belongs to exactly one tablet, the backfill process on any tablet can proceed independent of the others. The index build process is made efficient by running the build process for multiple tablets in parallel.

### Index backfill on a single tablet

For YCQL, index backfill for a tablet is handled in the tserver of that tablet (leader).  For YSQL, since we currently don't have sufficient context in tservers, a connection is opened up from tserver to postgres.  Postgres then takes the responsibility of reading the tablet and writing to the index table.

The following apply to both YCQL and YSQL:

* The index build requires a scan of the entire tablet data. However, there could be new updates happening on the dataset which would affect the values read by this scan. In order to prevent this, the scan is performed at a fixed timestamp. This hybrid logical timestamp `t_read` is picked by the YB-Master and sent to all the tablets. The data is scanned using this timestamp `t_read` as the read point so that subsequent writes do not affect the values read by this scan.

* The data is then scanned to generate the writes that need to be applied to the index table. These generated writes are batched and a batched write is performed to update the index table.

* It is important that the generated write entries being applied to the index table are written with a hybrid timestamp that is in the past, so that it is older than the hybrid timestamp of the new update operations that are running concurrent with the backfill process. These entries can either be written with one of the following hybrid logical timestamps (HTS):
    * The update time of the row being read
    * The timestamp `t_read` with which we are performing the scan

* Note that compactions for the Index table would not reclaim the delete markers until the backfill process is complete, i.e. until the index is in READ_WRITE_AND_DELETE state.

### Detecting constraint violations

A unique index will accept the writes only if **both** the following conditions hold true
1) Scan backwards in time and either:
    * there is no previous entry.
    * there is an entry and the immediately previous entry is a delete.
    * there is an entry and the immediately previous entry value matches the value being written.

2) Scan forward in time and either:
    * there is no next entry for that key.
    * there is an entry and the immediately next entry is a delete.
    * there is an entry and the immediately next entry value matches the value being written.

Requirement 1) is similar to what a unique index would do anyways. Condition 2) is require to detect cases where a concurrent insert/update - that violates uniqueness - may have been accepted; because the conflicting row was not backfilled. Having this criteria will help detect the conflict when the backfilled entry arrives after the concurrent write.


### Throttling index build rate

The rate at which the backfill should proceed can be specified by the desired number of rows of the primary table `MyTable` to process per second. In order to enforce this rate, the index backfill process keeps track of the number of rows being processed per second from the primary table `MyTable`. Note that this counter is maintained per backfill task.

Additionally, the maximum number of backfill operations happening on any YB-TServer across tablets can also be specified in order to rate-limit backfilling.

### Waiting for pending transactions to finish

So far, the discussion has made the assumption that all the concurrent updates (happening at the same time as the index build) on the `MyTable` table  are instantaneous operations. Specifically, this implies that:
* All the concurrent updates finish at a point in time
* The resulting updates on the index table (`MyIndex` in our example) can be handled based on its state as of that time (as determined by the value set in `IndexPermissions`).

However, this may not hold true for “transactions” where the write/index-permission checking is done at “apply” time. However the backfill algorithm, that may kick in later, will only see the “commit” time.

This means that if a write was “applied” before getting to update the index (wrt deleting the old value), and commits “after” the backfill timestamp is chosen, then neither operations may be updating the “index” to delete the overwritten value.

To guard against this case, the `GetSafeTime()` operation will wait for all “pending transactions” to finish (i.e. commit or abort) before determining the timestamp at which the scan is to be performed for backfill.

* Note that this strategy causes the backfill to “wait on” pending transactions, and if somebody has a terminal open with an ongoing txn that never commits. Backfill may be stalled indefinitely unless the very long running transaction is forcibly aborted.
* Add a timeout mechanism after which, pending txns (that started before getting to `WRITE_AND_DELETE` state) will be aborted.
* For user-enforced txns, there is no way for the tablet to know if a txn is in progress or not. The tablet will thus just “wait for a specific duration” which is a system flag `--index_backfill_upperbound_for_user_enforced_txn_duration_ms` and can be controlled by the user. In order to be safe, this value should be set to at least the RPC timeout (or a greater value).


# Fault tolerance using checkpointing

## Checkpointing

The YB-TServers, as a part of handling each of these RPC calls, will backfill **only a portion of the tablet’s key range**, and respond with a checkpointing info representing how far it managed to successfully build (much like a paging state). The YB-Master persists the checkpointing info in TabletInfo and issues futher RPCs as necessary for completing the entire key range for the tablet.

## Handling node restarts and leadership changes

The backfill process itself does not require that the job be restarted if there is a leadership change. The peer that has already started doing the backfill may be allowed to complete.


# Future Work

If a user creates multiple indices, the backfill for the different indices should be batched together so that only one scan is done. This would require the following:
* The user can create multiple indexes without the build immediately starting
* Backfill needs to be kicked off by the user using an explicit command -- say something along the lines of:
    ```
    BUILD/BACKFILL index <indexed_table>
    ```


[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/design/online-index-backfill.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
