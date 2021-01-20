---
title: Explicit Locking
headerTitle: Explicit Locking
linkTitle: Explicit Locking
description: Explicit Locking in YugabyteDB.
headcontent: Explicit Locking in YugabyteDB.
image: <div class="icon"><i class="fas fa-file-invoice-dollar"></i></div>
menu:
  latest:
    name: Explicit Locking
    identifier: explore-transactions-explicit-locking-1-ysql
    parent: explore-transactions
    weight: 245
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/multi-region-deployments/synchronous-replication-ysql/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

</ul>

This section explains how explicit locking works in YugabyteDB.

Explicit row-locks use transaction priorities to ensure that two transactions can never hold conflicting locks on the same row. This is done by the query layer assigning a very high value for the priority of the transaction that is being run to acquire the row lock, causing all other transactions that conflict with the current transaction to fail (because they have a lower value for the transaction priority). YugabyteDB supports most row-level locks, similar to PostgreSQL.

{{< note title="Note" >}}
Explicit locking is an area of active development in YugabyteDB. A number of enhancements are planned in this area. Unlike PostgreSQL, YugabyteDB uses optimistic concurrency control and does not block / wait for currently held locks, instead opting to abort the conflicting transaction with a lower priority. Pessimistic concurrency control is currently under development.
{{</note >}}

The types of row locks currently supported are: 
* `FOR UPDATE`
* `FOR NO KEY UPDATE`
* `FOR SHARE`
* `FOR KEY SHARE`

Let us look at en example with the `FOR UPDATE` row lock, where we first `SELECT` a row for update to lock it and subsequently update it. A concurrent transaction should not be able to abort this transaction by updating the value of that row after the row is locked. To try out this scenario, first create an example table with sample data as shown below.

```sql
yugabyte=# CREATE TABLE t (k VARCHAR, v VARCHAR);
yugabyte=# INSERT INTO t VALUES ('k1', 'v1');
```

Next, connect to the cluster using two independent `ysqlsh` instances called *session #1* and *session #2* below. 

{{< note title="Note" >}}
You can connect the session #1 and session #2 `ysqlsh` instances to the same server, or to different servers.
{{< /note >}}

<table style="margin:0 5px;">
  <tr>
   <td style="text-align:center;"><span style="font-size: 22px;">session #1</span></td>
   <td style="text-align:center; border-left:1px solid rgba(158,159,165,0.5);"><span style="font-size: 22px;">session #2</span></td>
  </tr>

  <tr>
    <td style="width:50%;">
    Begin a transaction in session #1. Perform a `SELECT FOR UPDATE` on the row in the table `t`, which will end up locking the row. This will cause the row to get locked for an update as a part of a transaction which has a very high priority.
    <pre><code style="padding: 0 10px;">
# Begin a new transaction in session #1
BEGIN;
# Lock key k1 for updates.
SELECT * from t WHERE k='k1' FOR UPDATE;
 k  | v
----+----
 k1 | v1
(1 row)
    </code></pre>
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5);">
    </td>
  </tr>

  <tr>
    <td style="width:50%;">
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5);">
    Before completing the transaction, try to update the same key in `session #2` using a simple update statement. This would use optimistic concurrency control, and therefore would fail right away. If `session #1` had used optimistic concurrency control instead of an explicit row-lock, then this update would succeed in some of the attempts and the transaction in `session #1` would fail in those cases.
    <pre><code style="padding: 0 10px;">
# Since row is locked by session #1,
# this update should fail.
UPDATE t SET v='v1.1' WHERE k='k1';
ERROR:  Operation failed. Try again. 
        xxx Conflicts with higher priority 
        transaction: yyy
    </code></pre>
{{< note title="Note" >}}
Blocking this transaction and retrying it after the other transaction completes is work in progress.
{{</note >}}
    </td>
  </tr>

  <tr>
    <td style="width:50%;">
    Update the row and commit the transaction in `session #1`. This should succeed.
    <pre><code style="padding: 0 10px;">
# Update should succeed since row
# was explicitly locked.
UPDATE t SET v='v1.2' WHERE k='k1';
# Commit fails.
COMMIT;
    </code></pre>
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5);">
    </td>
  </tr>

</table>







