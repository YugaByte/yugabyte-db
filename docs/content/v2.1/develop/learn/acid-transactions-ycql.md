---
title: ACID transactions in YCQL
headerTitle: ACID transactions
linkTitle: 4. ACID transactions
description: Learn how ACID transactions work in YCQL on YugabyteDB.
block_indexing: true
menu:
  v2.1:
    identifier: acid-transactions-1-ycql
    parent: learn
    weight: 566
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/develop/learn/acid-transactions-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/develop/learn/acid-transactions-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

A transaction is a sequence of operations performed as a single logical unit of work. A transaction has four key properties - **Atomicity**, **Consistency**, **Isolation** and **Durability** - commonly abbreviated as ACID.

- **Atomicity** All the work in a transaction is treated as a single atomic unit - either all of it is performed or none of it is.

- **Consistency** A completed transaction leaves the database in a consistent internal state. This can either be all the operations in the transactions succeeding or none of them succeeding.

- **Isolation** This property determines how/when changes made by one transaction become visible to the other. For example, a *serializable* isolation level guarantees that two concurrent transactions appear as if one executed after the other (i.e. as if they occur in a completely isolated fashion). YugabyteDB supports both *Snapshot Isolation*, and *Serializable* isolation levels. Read more about the different [levels of isolation](../../../architecture/transactions/isolation-levels/).

- **Durability** The results of the transaction are permanently stored in the system. The modifications must persist even in the instance of power loss or system failures.

## Creating the table

The table should be created with the `transactions` property enabled. The statement should look something as follows.

```sql
CREATE TABLE IF NOT EXISTS <TABLE_NAME> (...) WITH transactions = { 'enabled' : true };
```

### Java example

Here is an example of how to create a simple key-value table which has two columns with transactions enabled.

```java
String create_stmt =
  String.format("CREATE TABLE IF NOT EXISTS %s (k varchar, v varchar, primary key (k)) " +
                "WITH transactions = { 'enabled' : true };",
                tablename);
```

## Inserting or updating data

You can insert data by performing the sequence of commands inside a `BEGIN TRANSACTION` and `END TRANSACTION` block.

```sql
BEGIN TRANSACTION
  statement 1
  statement 2
END TRANSACTION;
```

### Java example

Here is a code snippet of how you would insert data into this table.

```java
// Insert two key values, (key1, value1) and (key2, value2) as a transaction.
String create_stmt = 
  String.format("BEGIN TRANSACTION" +
                "  INSERT INTO %s (k, v) VALUES (%s, %s);" +
                "  INSERT INTO %s (k, v) VALUES (%s, %s);" +
                "END TRANSACTION;",
                tablename, key1, value1,
                tablename, key2, value2;
```

## Prepare-bind transactions

You can prepare statements with transactions and bind variables to the prepared statements when executing the query.

### Java example

```java
String create_stmt = 
  String.format("BEGIN TRANSACTION" +
                "  INSERT INTO %s (k, v) VALUES (:k1, :v1);" +
                "  INSERT INTO %s (k, v) VALUES (:k1, :v2);" +
                "END TRANSACTION;",
                tablename, key1, value1,
                tablename, key2, value2;
PreparedStatement pstmt = client.prepare(create_stmt);

...

BoundStatement txn1 = pstmt.bind().setString("k1", key1)
                                  .setString("v1", value1)
                                  .setString("k2", key2)
                                  .setString("v2", value2);

ResultSet resultSet = client.execute(txn1);
```

## Sample Java Application

You can find a working example of using transactions with YugabyteDB in our [sample applications](../../../quick-start/run-sample-apps/). This application writes out string keys in pairs, with each pair of keys having the same value written as a transaction. There are multiple readers and writers that update and read these pair of keys. The number of reads and writes to perform can be specified as a parameter.

Here is how you can try out this sample application.

```sh
Usage:
  java -jar yb-sample-apps.jar \
    --workload CassandraTransactionalKeyValue \
    --nodes 127.0.0.1:9042

  Other options (with default values):
      [ --num_unique_keys 1000000 ]
      [ --num_reads -1 ]
      [ --num_writes -1 ]
      [ --value_size 0 ]
      [ --num_threads_read 24 ]
      [ --num_threads_write 2 ]
```

Browse the [Java source code for the batch application](https://github.com/yugabyte/yugabyte-db/blob/master/java/yb-loadtester/src/main/java/com/yugabyte/sample/apps/CassandraTransactionalKeyValue.java) to see how everything fits together.

## Example with ycqlsh

### Create keyspace and table

Create a keyspace.

```sql
ycqlsh> CREATE KEYSPACE banking;
```

Create a table with the `transactions` property set enabled.

```sql
ycqlsh> CREATE TABLE banking.accounts (
  account_name varchar,
  account_type varchar,
  balance float,
  PRIMARY KEY ((account_name), account_type)
) with transactions = { 'enabled' : true };
```

You can verify that this table has transactions enabled on it by running the following query.

```sql
ycqlsh> select keyspace_name, table_name, transactions from system_schema.tables
where keyspace_name='banking' AND table_name = 'accounts';
```

```
 keyspace_name | table_name | transactions
---------------+------------+---------------------
       banking |   accounts | {'enabled': 'true'}

(1 rows)
```

### Insert sample data

Let us seed this table with some sample data.

```sql
INSERT INTO banking.accounts (account_name, account_type, balance) VALUES ('John', 'savings', 1000);
INSERT INTO banking.accounts (account_name, account_type, balance) VALUES ('John', 'checking', 100);
INSERT INTO banking.accounts (account_name, account_type, balance) VALUES ('Smith', 'savings', 2000);
INSERT INTO banking.accounts (account_name, account_type, balance) VALUES ('Smith', 'checking', 50);
```

Here are the balances for John and Smith.

```sql
ycqlsh> select * from banking.accounts;
```

```
 account_name | account_type | balance
--------------+--------------+---------
         John |     checking |     100
         John |      savings |    1000
        Smith |     checking |      50
        Smith |      savings |    2000
```

Check John's balance.

```sql
ycqlsh> SELECT SUM(balance) as Johns_balance FROM banking.accounts WHERE account_name='John';
```

```
 johns_balance
---------------
          1100
```

Check Smith's balance.

```sql
ycqlsh> SELECT SUM(balance) as smiths_balance FROM banking.accounts WHERE account_name='Smith';
```

```
 smiths_balance
----------------
           2050

```

### Execute a transaction

Here are a couple of examples of executing transactions.

Let us say John transfers $200 from his savings account to his checking account. This has to be a transactional operation. This can be achieved as follows.

```sql
BEGIN TRANSACTION
  UPDATE banking.accounts SET balance = balance - 200 WHERE account_name='John' AND account_type='savings';
  UPDATE banking.accounts SET balance = balance + 200 WHERE account_name='John' AND account_type='checking';
END TRANSACTION;
```

If you now selected the value of John's account, you should see the amounts reflected. The total balance should be the same $1100 as before.

```sql
ycqlsh> select * from banking.accounts where account_name='John';
```

```
 account_name | account_type | balance
--------------+--------------+---------
         John |     checking |     300
         John |      savings |     800
```

Check John's balance.

```sql
ycqlsh> SELECT SUM(balance) as Johns_balance FROM banking.accounts WHERE account_name='John';
```

```
 johns_balance
---------------
          1100
```

Further, the checking and savings account balances for John should have been written at the same write timestamp.

```sql
ycqlsh> select account_name, account_type, balance, writetime(balance) 
from banking.accounts where account_name='John';
```

```
 account_name | account_type | balance | writetime(balance)
--------------+--------------+---------+--------------------
         John |     checking |     300 |   1517898028890171
         John |      savings |     800 |   1517898028890171
```

Now let us say John transfers the $200 from his checking account to Smith's checking account. We can accomplish that with the following transaction.

```sql
BEGIN TRANSACTION
  UPDATE banking.accounts SET balance = balance - 200 WHERE account_name='John' AND account_type='checking';
  UPDATE banking.accounts SET balance = balance + 200 WHERE account_name='Smith' AND account_type='checking';
END TRANSACTION;
```

We can verify the transfer was made as we intended, and also verify that the time at which the two accounts were updated are identical by performing the following query.

```sql
ycqlsh> select account_name, account_type, balance, writetime(balance) from banking.accounts;
```

```
 account_name | account_type | balance | writetime(balance)
--------------+--------------+---------+--------------------
         John |     checking |     100 |   1517898167629366
         John |      savings |     800 |   1517898028890171
        Smith |     checking |     250 |   1517898167629366
        Smith |      savings |    2000 |   1517894361290020
```

The net balance for John should have decreased by $200 which that of Smith should have increased by $200.

```sql
ycqlsh> SELECT SUM(balance) as Johns_balance FROM banking.accounts WHERE account_name='John';
```

```
 johns_balance
---------------
           900
```

Check Smith's balance.

```sql
ycqlsh> SELECT SUM(balance) as smiths_balance FROM banking.accounts WHERE account_name='Smith';
```

```
 smiths_balance
----------------
           2250
```

## Note on linearizability

By default, the original Cassandra Java driver and the YugabyteDB Cassandra Java driver use `com.datastax.driver.core.policies.DefaultRetryPolicy` which can retry requests upon timeout on client side.

Automatic retries can break linearizability of operations from the client point of view. Therefore you have added `com.yugabyte.driver.core.policies.NoRetryOnClientTimeoutPolicy` which inherits behavior from DefaultRetryPolicy with one exception - it results in an error in case the operation times out (with the `OperationTimedOutException`).

Under network partitions, this can lead to the case when client gets a successful response to retried request and treats the operation as completed, but the value might get overwritten by an older operation due to retries.

To avoid such linearizability issues, use `com.yugabyte.driver.core.policies.NoRetryOnClientTimeoutPolicy` and handle client timeouts in the application layer.
