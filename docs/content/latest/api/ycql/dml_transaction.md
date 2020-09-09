---
title: TRANSACTION statement [YCQL]
headerTitle: TRANSACTION
linkTitle: TRANSACTION
description: Use the TRANSACTION statement block to make changes to multiple rows in one or more tables in a distributed ACID transaction.
menu:
  latest:
    parent: api-cassandra
    weight: 1330
aliases:
  - /latest/api/cassandra/dml_transaction
  - /latest/api/ycql/dml_transaction
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the TRANSACTION statement block to make changes to multiple rows in one or more tables in a [distributed ACID transaction](../../../architecture/transactions/distributed-txns).

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="207" height="180" viewbox="0 0 207 180"><path class="connector" d="M0 22h5m59 0h10m106 0h5m-185 50h25m-5 0q-5 0-5-5v-17q0-5 5-5h146q5 0 5 5v17q0 5-5 5m-141 0h20m54 0h27m-91 25q0 5 5 5h5m61 0h5q5 0 5-5m-86-25q5 0 5 5v50q0 5 5 5h5m56 0h10q5 0 5-5v-50q0-5 5-5m5 0h10m25 0h25m-186 95h5m46 0h10m106 0h10m25 0h5"/><rect class="literal" x="5" y="5" width="59" height="25" rx="7"/><text class="text" x="15" y="22">BEGIN</text><rect class="literal" x="74" y="5" width="106" height="25" rx="7"/><text class="text" x="84" y="22">TRANSACTION</text><a xlink:href="../grammar_diagrams#insert"><rect class="rule" x="45" y="55" width="54" height="25"/><text class="text" x="55" y="72">insert</text></a><a xlink:href="../grammar_diagrams#update"><rect class="rule" x="45" y="85" width="61" height="25"/><text class="text" x="55" y="102">update</text></a><a xlink:href="../grammar_diagrams#delete"><rect class="rule" x="45" y="115" width="56" height="25"/><text class="text" x="55" y="132">delete</text></a><rect class="literal" x="136" y="55" width="25" height="25" rx="7"/><text class="text" x="146" y="72">;</text><rect class="literal" x="5" y="150" width="46" height="25" rx="7"/><text class="text" x="15" y="167">END</text><rect class="literal" x="61" y="150" width="106" height="25" rx="7"/><text class="text" x="71" y="167">TRANSACTION</text><rect class="literal" x="177" y="150" width="25" height="25" rx="7"/><text class="text" x="187" y="167">;</text></svg>

### Grammar

```
transaction_block ::= BEGIN TRANSACTION
                          ( insert | update | delete ) ';'
                          [ ( insert | update | delete ) ';' ...]
                      END TRANSACTION ';'
```

Where

- `insert`, `update`, and `delete` are [INSERT](../dml_insert), [UPDATE](../dml_update), and [DELETE](../dml_delete) statements.

### ANSI SQL syntax

Alternatively, YugabyteDB supports ANSI SQL `START TRANSACTION` and `COMMIT` statements.

```
transaction_block ::= START TRANSACTION ';'
                      ( insert | update | delete ) ';'
                      [ ( insert | update | delete ) ';' ...]
                      COMMIT ';'
```

## Semantics

- An error is raised if transactions are not enabled in any of the tables inserted, updated, or deleted.
- Currently, an error is raised if any of the `INSERT`, `UPDATE`, or `DELETE` statements contains an `IF` clause.

## Examples

### Create a table with transactions enabled

```sql
ycqlsh:example> CREATE TABLE accounts (account_name TEXT,
                                      account_type TEXT,
                                      balance DOUBLE,
                                      PRIMARY KEY ((account_name), account_type))
               WITH transactions = { 'enabled' : true };
```

### Insert some data

```sql
ycqlsh:example> INSERT INTO accounts (account_name, account_type, balance)
               VALUES ('John', 'savings', 1000);
ycqlsh:example> INSERT INTO accounts (account_name, account_type, balance)
               VALUES ('John', 'checking', 100);
ycqlsh:example> INSERT INTO accounts (account_name, account_type, balance)
               VALUES ('Smith', 'savings', 2000);
ycqlsh:example> INSERT INTO accounts (account_name, account_type, balance)
               VALUES ('Smith', 'checking', 50);
```

```sql
ycqlsh:example> SELECT account_name, account_type, balance, writetime(balance) FROM accounts;
```

```
 account_name | account_type | balance | writetime(balance)
--------------+--------------+---------+--------------------
         John |     checking |     100 |   1523313964356489
         John |      savings |    1000 |   1523313964350449
        Smith |     checking |      50 |   1523313964371579
        Smith |      savings |    2000 |   1523313964363056
```

### Update 2 rows with the same partition key

You can do this as shown below.

```sql
ycqlsh:example> BEGIN TRANSACTION
                 UPDATE accounts SET balance = balance - 200 WHERE account_name = 'John' AND account_type = 'savings';
                 UPDATE accounts SET balance = balance + 200 WHERE account_name = 'John' AND account_type = 'checking';
               END TRANSACTION;
```

```sql
ycqlsh:example> SELECT account_name, account_type, balance, writetime(balance) FROM accounts;
```

```
 account_name | account_type | balance | writetime(balance)
--------------+--------------+---------+--------------------
         John |     checking |     300 |   1523313983201270
         John |      savings |     800 |   1523313983201270
        Smith |     checking |      50 |   1523313964371579
        Smith |      savings |    2000 |   1523313964363056
```

### Update 2 rows with the different partition keys

```sql
ycqlsh:example> BEGIN TRANSACTION
                 UPDATE accounts SET balance = balance - 200 WHERE account_name = 'John' AND account_type = 'checking';
                 UPDATE accounts SET balance = balance + 200 WHERE account_name = 'Smith' AND account_type = 'checking';
               END TRANSACTION;
```

```sql
ycqlsh:example> SELECT account_name, account_type, balance, writetime(balance) FROM accounts;
```

```
 account_name | account_type | balance | writetime(balance)
--------------+--------------+---------+--------------------
         John |     checking |     100 |   1523314002218558
         John |      savings |     800 |   1523313983201270
        Smith |     checking |     250 |   1523314002218558
        Smith |      savings |    2000 |   1523313964363056
```



{{< note Type="Note" >}}
`BEGIN/END TRANSACTION` doesn't currently support `RETURNS STATUS AS ROW`. 
{{< /note >}}


## See also

- [`INSERT`](../dml_insert)
- [`UPDATE`](../dml_update)
- [`DELETE`](../dml_delete)
