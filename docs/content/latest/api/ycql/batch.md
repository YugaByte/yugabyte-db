---
title: BATCH requests [YCQL]
headerTitle: BATCH
linkTitle: BATCH
summary: Execute multiple DML in 1 request
description: Use batch to update multiple rows in 1 request.
menu:
  latest:
    parent: api-cassandra
    weight: 19991
isTocNested: true
showAsideToc: true
---

Batch operations let you send multiple operations in a single RPC call to the database. The larger the batch size, 
the higher the latency for the entire batch. Although the latency for the entire batch of operations is higher than the latency of any single operation, 
the throughput of the batch of operations is much higher.

## Example in Java

To perform a batch insert operation in Java:

1.  Create a BatchStatement object. 
2. Add the desired number of prepared and bound insert statements to it. 
3. Execute the batch object. 

```java
// Create a batch statement object.
BatchStatement batch = new BatchStatement();

// Create a prepared statement object to add to the batch.
PreparedStatement insert = client.prepare("INSERT INTO table (k, v) VALUES (?, ?)");

// Bind values to the prepared statement and add them to the batch.
for (...) {
  batch.add(insert.bind( ... <values for bind variables> ... ));
}

// Execute the batch operation.
ResultSet resultSet = client.execute(batch);

```
## Example in Python using RETURNS AS STATUS

An example using Python client and RETURNS AS STATUS clause:

```sql
ycqlsh> create keyspace if not exists yb_demo;
ycqlsh> CREATE TABLE if not exists yb_demo.test_rs_batch(h int, r bigint, v1 int, v2 varchar, primary key (h, r));
ycqlsh> INSERT INTO yb_demo.test_rs_batch(h,r,v1,v2) VALUES (1,1,1,'a');
ycqlsh> INSERT INTO yb_demo.test_rs_batch(h,r,v2) VALUES (3,3,'b');
ycqlsh> select * from yb_demo.test_rs_batch;

 h | r | v1   | v2
---+---+------+----
 1 | 1 |    1 |  a
 3 | 3 | null |  b

(2 rows)
```

Getting status from DML operations in Python:

```python
from cassandra.cluster import Cluster, BatchStatement

# Create a cluster and a session
cluster = Cluster(['127.0.0.1'])
session = cluster.connect()

# Create a batch statement object.
b = BatchStatement()

# Add multiple queries
b.add(f"INSERT INTO test_rs_batch(h, r, v1, v2) VALUES (1, 1, 1 ,'a') RETURNS STATUS AS ROW")
b.add(f"UPDATE test_rs_batch SET v2='z' WHERE h=3 AND r=3 IF v2='z'  RETURNS STATUS AS ROW")
b.add(f"DELETE FROM test_rs_batch WHERE h=2 AND r=2 IF EXISTS RETURNS STATUS AS ROW")

# Execute the batch operation.
result = session.execute(b, trace=True)

# Print status for each DML operation
for row in result:
    print(row)
```

The output generated is:

```python
Row(applied=True, message=None, h=None, r=None, v1=None, v2=None)
Row(applied=False, message=None, h=3, r=3, v1=None, v2='b')
Row(applied=False, message=None, h=None, r=None, v1=None, v2=None)
```

## Row Status

When executing a batch in YCQL, the protocol allows returning only one error or return status.

If one statement fails with an error or for conditional DMLs, some are not applied because of failing the IF condition, the driver or application cannot accurately identify the relevant statements, it will just receive one general error or return-status for the batch.

Therefore, it is not possible for an application to react to such failures appropriately (for example, retry, abort, 
and change some parameters for either the entire batch or just the relevant statements).

You can address this limitation by using the `RETURNS STATUS AS ROW` feature.


If used, the write statement will return its status (whether applied, unapplied, or errored-out with a message) as a regular CQL row that the application can inspect and decide what to do.

For a batch, it is required that either none or all statements use `RETURNS STATUS AS ROW`.


When executing `n` statements in a batch with `RETURN STATUS AS ROW`, `n` rows are returned, in the same order as the statements and the application can easily inspect the result.

For batches containing conditional DMLs, `RETURN STATUS AS ROW` must be used.

For conditional DMLs (not normally allowed in batches), any subset of them could fail due to 
their `IF` condition and thus returning rows only for them makes it impossible to identify which ones actually failed.

To distinguish between the two not-applied cases (error vs condition is false), there is an error `message` column in the return row that will be null for not-applied and filled-in for errors.

Conversely, there will be one column for each table column, which will be `null` for errors but filled-in for not-applied (justifying the decision to not apply).

For instance:

1. Set up a simple table:
```sql
cqlsh:sample> CREATE TABLE test(h INT, r INT, v LIST<INT>, PRIMARY KEY(h,r)) WITH transactions={'enabled': true};
cqlsh:sample> INSERT INTO test(h,r,v) VALUES (1,1,[1,2]);
```

2. Unapplied update when `IF` condition is false:
```sql
cqlsh:sample> UPDATE test SET v[2] = 4 WHERE h = 1 AND r = 1 IF v[1] = 3 RETURNS STATUS AS ROW;
 [applied] | [message] | h | r | v
-----------+-----------+---+---+--------
     False |      null | 1 | 1 | [1, 2]
```

3. Unapplied update when `IF` condition true but error:
```sql
cqlsh:sample> UPDATE test SET v[20] = 4 WHERE h = 1 AND r = 1 IF v[1] = 2 RETURNS STATUS AS ROW;
 [applied] | [message]                                                                              | h    | r    | v
-----------+----------------------------------------------------------------------------------------+------+------+------
     False | Unable to replace items into list, expecting index 20, reached end of list with size 2 | null | null | null
```
4. Applied update when `IF` condition true:
```sql
cqlsh:sample> UPDATE test SET v[0] = 4 WHERE h = 1 AND r = 1 IF v[1] = 2 RETURNS STATUS AS ROW;
 [applied] | [message] | h    | r    | v
-----------+-----------+------+------+------
      True |      null | null | null | null
```
5. Final table result:
```sql
cqlsh:sample> SELECT * FROM test;
 h | r | v
---+---+--------
 1 | 1 | [4, 2]
(1 rows)
```

{{< note Type="Note" >}}

`BEGIN/END TRANSACTION` doesn't currently support `RETURNS STATUS AS ROW`. 

{{< /note >}}
