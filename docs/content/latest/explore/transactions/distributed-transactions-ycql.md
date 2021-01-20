---
title: Distributed Transactions
headerTitle: Distributed Transactions
linkTitle: Distributed Transactions
description: Distributed Transactions in YugabyteDB.
headcontent: Distributed Transactions in YugabyteDB.
menu:
  latest:
    name: Distributed Transactions
    identifier: explore-transactions-distributed-transactions-2-ycql
    parent: explore-transactions
    weight: 230
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/transactions/distributed-transactions-ysql/" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/explore/transactions/distributed-transactions-ycql/" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

## Creating the table

Create a keyspace.

```sql
ycqlsh> CREATE KEYSPACE banking;
```

The YCQL table should be created with the `transactions` property enabled. The statement should look something as follows.

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

## Insert sample data

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

## Execute a transaction

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

## Verify

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
