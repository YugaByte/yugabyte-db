---
title: Explicit locking
headerTitle: Explicit locking
linkTitle: Explicit locking
description: Learn about support for explicit locks in YugabyteDB.
menu:
  latest:
    identifier: architecture-transactions-explicit-locking
    parent: architecture-acid-transactions
    weight: 1153
isTocNested: true
showAsideToc: true
---

This section explains how explicit locking works in YugabyteDB. The transactions layer of YugabyteDB can support both optimistic and pessimistic locks.

## Concurrency control

[Concurrency control](https://en.wikipedia.org/wiki/Concurrency_control) in databases ensures that multiple transactions can execute concurrently while preserving data integrity. Concurrency control is essential for correctness in environments where two or more transactions can access the same data at the same time.

The two primary mechanisms to achieve concurrency control are *optimistic* and *pessimistic*. Concurrency control in YugabyteDB can accommodate both of these depending on the scenario.


DocDB exposes the ability to write [provisional records]() which is exercised by the query layer. Provisional records are used to order persist locks on rows in order to detect conflicts. Provisional records have a *priority* assosciated with them, which is a number. When two transactions conflict, the transaction with the lower priority is aborted.

### Optimistic concurrency control

[Optimistic locking](https://en.wikipedia.org/wiki/Optimistic_concurrency_control) delays the checking of whether a transaction meets the isolation and other integrity rules until its end, without blocking any of the operations performed as a part of the transaction. In scenarios where there are two concurrent transactions that conflict with each other (meaning a commit of the changes made by both these transactions would violate integrity constraints), one of these transactions is aborted. An aborted transaction could immediately be restarted and re-executed, or surfaced as an error to the end user.

In scenarios where too many transactions do not conflict with each other, optimistic concurrency control is a good strategy. This is generally the case in high-volume systems. For example, most web applications have short-lived the connections to the database.

YugabyteDB opts for optimistic concurrency in the case of simple transactions. This is achieved by assigning a random priority to each of the transactions. In the case of a conflict, the transaction with a lower priority is aborted. Some transactions that get aborted due to a conflict are internally retried while others result in an error to the end application.

### Pessimistic concurrency control

* **Pessimistic** - Block an operation of a transaction, if it may cause violation of the rules, until the possibility of violation disappears. Blocking operations is typically involved with performance reduction.

[Pessimistic locking](https://en.wikipedia.org/wiki/Record_locking) blocks a transaction if any of its operations would violate relational integrity if it executed. This means that as long as the first transaction that locked a row has not completed (either `COMMIT` or `ABORT`), no other transaction would be able to lock that row.

{{< note title="Note" >}}
YugabyteDB currently supports optimistic concurrency control, with pessimistic concurrency control being worked on actively.
{{</note >}}

Pessimistic locking is good when there are longer running operations that would increase the probability of transaction conflicts. For example, if there are multiple concurrent transactions that update many rows in the database and conflict with one another, these transactions could continuously get aborted because they conflict with one another. Pessimistic locking allows these transaction to make progress and complete by avoiding these conflicts. 

Here is another way to understand *optimistic* versus *pessimistic* concurrency control. Optimistic concurrency control incurs an overhead only if there are conflicts. Most OLTP applications typically have short-lived transactions that would not conflict. Pessimistic concurrency control decreases the overhead incurred when conflicts occur.


### Deadlock detection

When using pessimistic locks, there could be a possibility of introducing [deadlocks](https://en.wikipedia.org/wiki/Record_locking) into the execution of the system.

> The introduction of granular (subset) locks creates the possibility for a situation called deadlock. Deadlock is possible when incremental locking (locking one entity, then locking one or more additional entities) is used. To illustrate, if two bank customers asked two clerks to obtain their account information so they could transfer some money into other accounts, the two accounts would essentially be locked. Then, if the customers told their clerks that the money was to be transferred into each other's accounts, the clerks would search for the other accounts but find them to be "in use" and wait for them to be returned. Unknowingly, the two clerks are waiting for each other, and neither of them can complete their transaction until the other gives up and returns the account. 

YugabyteDB currently avoids deadlocks because of its transaction conflict handling semantics, where the transaction with the lower priority is completely aborted.


## Row-level locks

YugabyteDB supports most row-level locks, similar to PostgreSQL. However, one difference is that YugabyteDB uses optimistic concurrency control and does not block / wait for currently held locks, instead opting to abort the conflicting transaction with a lower priority. Note that pessimistic concurrency control is under works.

Explicit row-locks use transaction priorities to ensure that two transactions can never hold conflicting locks on the same row. This is done by the query layer assigning a very high value for the priority of the transaction that is being run under pessimistic concurrency control. This has the effect of causing all other transactions that conflict with the current transaction to fail, because they have a lower value for the transaction priority.


A list of lock modes supported is shown below. Row-level locks do not affect querying data. They only block performing writes and obtaining locks to the locked row.

There is no limit on the number of rows that can be locked at a time. Row locks are not stored in memory, they result in writes to the disk.


### Types of row-level locks

##### `FOR UPDATE`

The `FOR UPDATE` lock causes the rows retrieved by the `SELECT` statement to be locked as though for an update. This prevents these rows from being subsequently locked, modified or deleted by other transactions until the current transaction ends. The following operations performed on a previously locked row as a part of other transactions will fail: `UPDATE`, `DELETE`, `SELECT FOR UPDATE`, `SELECT FOR NO KEY UPDATE`, `SELECT FOR SHARE` or `SELECT FOR KEY SHARE`.


{{< note title="Note" >}}

Unlike PostgreSQL, the operations on a previously locked row do not currently block in YugabyteDB until the transaction holding a lock finishes. This work is planned and will be the behavior in a future release.

{{</note >}}

The `FOR UPDATE` lock mode is also acquired by any `DELETE` on a row, and also by an `UPDATE` that modifies the values on certain columns.

##### `FOR NO KEY UPDATE`

Behaves similarly to `FOR UPDATE`, except that the lock acquired is weaker: this lock will not block `SELECT FOR KEY SHARE` commands that attempt to acquire a lock on the same rows. This lock mode is also acquired by any `UPDATE` that does not acquire a `FOR UPDATE` lock.

##### `FOR SHARE`

Behaves similarly to `FOR NO KEY UPDATE`, except that it acquires a shared lock rather than exclusive lock on each retrieved row. A shared lock blocks other transactions from performing `UPDATE`, `DELETE`, `SELECT FOR UPDATE` or `SELECT FOR NO KEY UPDATE` on these rows, but it does not prevent them from performing `SELECT FOR SHARE` or `SELECT FOR KEY SHARE`.

##### `FOR KEY SHARE`

Behaves similarly to `FOR SHARE`, except that the lock is weaker: `SELECT FOR UPDATE` is blocked, but not `SELECT FOR NO KEY UPDATE`. A key-shared lock blocks other transactions from performing `DELETE` or any `UPDATE` that changes the key values, but not other `UPDATE`, and neither does it prevent `SELECT FOR NO KEY UPDATE`, `SELECT FOR SHARE`, or `SELECT FOR KEY SHARE`.


{{< note title="Note" >}}

YugabyteDB still uses optimistic locking in the case of `FOR KEY SHARE`. Making this pessimistic is work in progress.

{{</note >}}

### Example

As an example, connect to a YugabyteDB cluster using `ysqlsh`. Create a table `t` and insert one row into it as shown below.

```postgres
yugabyte=# CREATE TABLE t (k VARCHAR, v VARCHAR);
yugabyte=# INSERT INTO t VALUES ('k1', 'v1');
```

Next, connect two different instances of the ysqlsh shell to YugabyteDB. We will refer to these as `session #1` and `session #2` below.

1. Run the following in `session #1` first. The example below uses an explicit row-level lock using `SELECT FOR UPDATE`, which use pessimistic concurrency control.

```
# SESSION #1

# Begin a new transaction in session #1
BEGIN;

# Lock key k1 for updates.
SELECT * from t WHERE k='k1' FOR UPDATE;

 k  | v
----+----
 k1 | v1
(1 row)
```

2. Before completing the transaction, try to update the same key in `session #2` using a simple update statement. This would use optimistic concurrency control, and therefore would fail right away. Seamlessly retrying this operation internally is a work in progress.

```
# SESSION #2

# Since row is locked by session #1, this update should fail.
UPDATE t SET v='v1.1' WHERE k='k1';

ERROR:  Operation failed. Try again.: xxx Conflicts with higher priority transaction: yyy
```

{{< note title="Note" >}}

If `session #1` had used optimistic concurrency control instead of an explicit row-lock, then this update would succeed in some of the attempts and the transaction in `session #1` would fail in those cases.

{{</note >}}


3. Update the row and commit the transaction in `session #1`. This should succeed.

```
# SESSION #1

# Update should succeed since row was explicitly locked.
UPDATE t SET v='v1.2' WHERE k='k1';
UPDATE 1


# Commit fails.
COMMIT;

```
