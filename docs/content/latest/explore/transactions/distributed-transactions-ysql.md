---
title: Distributed Transactions
headerTitle: Distributed Transactions
linkTitle: Distributed Transactions
description: Distributed Transactions in YugabyteDB.
headcontent: Distributed Transactions in YugabyteDB.
menu:
  latest:
    name: Distributed Transactions
    identifier: explore-transactions-distributed-transactions-1-ysql
    parent: explore-transactions
    weight: 230
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/transactions/distributed-transactions-ysql/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/explore/transactions/distributed-transactions-ycql/" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Overview of transaction control

This section explains how a distributed transaction works in YugabyteDB. We will use the example table below to describe the control flow of a simple transaction.

```sql
CREATE TABLE accounts (
  account_name varchar,
  account_type varchar,
  balance float,
  PRIMARY KEY (account_name, account_type)
);
```

Insert some sample data into the table.

```sql
INSERT INTO accounts VALUES ('John', 'savings', 1000);
INSERT INTO accounts VALUES ('John', 'checking', 100);
INSERT INTO accounts VALUES ('Smith', 'savings', 2000);
INSERT INTO accounts VALUES ('Smith', 'checking', 50);
```

The table should look as follows:

```
yugabyte=# select * from accounts;
 account_name | account_type | balance
--------------+--------------+---------
 John         | checking     |     100
 John         | savings      |    1000
 Smith        | checking     |      50
 Smith        | savings      |    2000
(4 rows)
```

Now, we will run the following transaction and explain what happens at each step.

```sql
BEGIN TRANSACTION;
  UPDATE accounts SET balance = balance - 200 
      WHERE account_name='John' AND account_type='savings';
  UPDATE accounts SET balance = balance + 200 
      WHERE account_name='John' AND account_type='checking';
COMMIT;
```


<table style="margin:0 5px;">
  <tr>
   <td style="text-align:center;"><span style="font-size: 22px;">Command</span></td>
   <td style="text-align:center; border-left:1px solid rgba(158,159,165,0.5);"><span style="font-size: 22px;">Description</span></td>
  </tr>

  <tr>
    <td style="width:50%;">
    <pre><code style="padding: 0 10px;">
BEGIN TRANSACTION;
    </code></pre>
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5); font-size: 16px;">
      The node that receives this statement becomes the transaction coordinator. A new transaction record is created in the <code>transaction status</code> table for the current transaction. It has a unique transaction id with the state <code>PENDING</code>. Note that in practice, these records are pre-created to achieve high performance.
    </td>
  </tr>

  <tr>
    <td style="width:50%;">
    <pre><code style="padding: 0 10px;">
UPDATE accounts SET balance = balance - 200 
  WHERE account_name='John' 
  AND account_type='savings';
    </code></pre>
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5); font-size: 16px;">
      The transaction coordinator writes a *provisional record* to the tablet that contains this row. The provisional record consists of the transaction id, so the state of the transaction can be determined. If there already exists a provisional record written by another transaction, then the current transaction would use the transaction id that is present in the provisional record to fetch details and check if there is a potential conflict.  
    </td>
  </tr>

  <tr>
    <td style="width:50%;">
    <pre><code style="padding: 0 10px;">
UPDATE accounts SET balance = balance + 200 
  WHERE account_name='John' 
  AND account_type='checking';
    </code></pre>
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5); font-size: 16px;">
      This step is largely the same as the previous step. Note that the rows being accessed can live on different nodes. The transaction coordinator would need to perform a provisional write RPC to the appropriate node for each row.
    </td>
  </tr>

  <tr>
    <td style="width:50%;">
    <pre><code style="padding: 0 10px;">
COMMIT;
    </code></pre>
    </td>
    <td style="width:50%; border-left:1px solid rgba(158,159,165,0.5); font-size: 16px;">
      Note that in order to <code>COMMIT</code>, all the provisional writes must have successfully completed. The <code>COMMIT</code> statement causes the transaction coordinator to update the transaction status in the <code>transaction status</code> table to <code>COMMITED</code>, at which point it is assigned the commit timestamp (which is a *hybrid timestamp* to be precise). At this point, the transaction is completed. In the background, the <code>COMMIT</code> record along with the commit timestamp is applied to each of the rows that participated to make future lookups of these rows efficient.
    </td>
  </tr>

</table>

This is shown diagrammatically below.
![distributed_txn_write_path](/images/architecture/txn/distributed_txn_write_path.svg)


After the above transaction succeeds, the table should look as follows.

```
yugabyte=# select * from accounts;
 account_name | account_type | balance
--------------+--------------+---------
 John         | checking     |     300
 John         | savings      |     800
 Smith        | checking     |      50
 Smith        | savings      |    2000
(4 rows)
```


### Scalability

Since all nodes of the cluster can process transactions by becoming transaction coordinators, horizontal scalability can simply be achieved by distributing the queries evenly across the nodes of the cluster.

### Resilience

Each update performed as a part of the transaction is replicated across multiple nodes for resilience and persisted on disk. This ensures that the updates made as a part of a transaction that has been acknowledged as successful to the end user is resilient even if failures occur.

### Concurrency control 

[Concurrency control](https://en.wikipedia.org/wiki/Concurrency_control) in databases ensures that multiple transactions can execute concurrently while preserving data integrity. Concurrency control is essential for correctness in environments where two or more transactions can access the same data at the same time. The two primary mechanisms to achieve concurrency control are *optimistic* and *pessimistic*. 

{{< note title="Note" >}}
YugabyteDB currently supports optimistic concurrency control, with pessimistic concurrency control being worked on actively.
{{</note >}}


## Transaction Options

You can see the various options supported by transactions by running the `\h BEGIN` statement, as shown below.

```
yugabyte=# \h BEGIN
Command:     BEGIN
Description: start a transaction block
Syntax:
BEGIN [ WORK | TRANSACTION ] [ transaction_mode [, ...] ]

where transaction_mode is one of:

  ISOLATION LEVEL { SERIALIZABLE | REPEATABLE READ | READ COMMITTED | READ UNCOMMITTED }
  READ WRITE | READ ONLY
  [ NOT ] DEFERRABLE
```

### `transaction_mode`

The `transaction_mode` can be set to one of the following options:

1. `READ WRITE` – Users can perform read or write operations.
2. `READ ONLY` – Users can perform read operations only.

As an example, trying to do a write operation such as creating a table or inserting a row in a `READ ONLY` transaction would result in an error as shown below.

```
yugabyte=# BEGIN READ ONLY;
BEGIN

yugabyte=# CREATE TABLE example(k INT PRIMARY KEY);
ERROR: 25P02: current transaction is aborted, commands ignored until end of 
              transaction block
```

### `DEFERRABLE` transactions

The `DEFERRABLE` transaction property in YSQL is similar to PostgreSQL in that has no effect unless the transaction is also `SERIALIZABLE` and `READ ONLY`. 

When all three of these properties (`SERIALIZABLE`, `DEFERRABLE` and `READ ONLY`) are set for a transaction, the transaction may block when first acquiring its snapshot, after which it is able to run without the normal overhead of a `SERIALIZABLE` transaction and without any risk of contributing to or being canceled by a serialization failure.

{{< tip title="Tip" >}}
This mode is well suited for long-running reports or backups without being impacting or impacted by other transactions.
{{< /tip >}}

