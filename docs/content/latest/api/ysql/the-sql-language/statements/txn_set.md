---
title: SET TRANSACTION statement [YSQL]
headerTitle: SET TRANSACTION
linkTitle: SET TRANSACTION
description: Use the `SET TRANSACTION` statement to set the current transaction isolation level.
summary: SET TRANSACTION
menu:
  latest:
    identifier: txn_set
    parent: statements
aliases:
  - /latest/api/ysql/commands/txn_set/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SET TRANSACTION` statement to set the current transaction isolation level.

## Syntax

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#grammar" class="nav-link active" id="grammar-tab" data-toggle="tab" role="tab" aria-controls="grammar" aria-selected="true">
      <i class="fas fa-file-alt" aria-hidden="true"></i>
      Grammar
    </a>
  </li>
  <li>
    <a href="#diagram" class="nav-link" id="diagram-tab" data-toggle="tab" role="tab" aria-controls="diagram" aria-selected="false">
      <i class="fas fa-project-diagram" aria-hidden="true"></i>
      Diagram
    </a>
  </li>
</ul>

<div class="tab-content"> 
  <div id="grammar" class="tab-pane fade show active" role="tabpanel" aria-labelledby="grammar-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set_transaction,transaction_mode,isolation_level,read_write_mode,deferrable_mode.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/set_transaction,transaction_mode,isolation_level,read_write_mode,deferrable_mode.diagram.md" /%}}
  </div>
</div>

## Semantics

Supports both Serializable and Snapshot Isolation using the PostgreSQL isolation level syntax of `SERIALIZABLE` and `REPEATABLE READ` respectively. Even `READ COMMITTED` and `READ UNCOMMITTED` isolation levels are mapped to Snapshot Isolation.

### *transaction_mode*

Set the transaction mode to one of the following.

- `ISOLATION LEVEL` clause
- Access mode
- `DEFERRABLE` mode

### ISOLATION LEVEL clause

#### SERIALIZABLE

Default in ANSI SQL standard.

#### REPEATABLE READ

Also referred to as "snapshot isolation" in YugabyteDB.
Default.

#### READ COMMITTED

A statement can only see rows committed before it begins.

`READ COMMITTED` is mapped to `REPEATABLE READ`.

Default in PostgreSQL.

#### READ UNCOMMITTED

`READ UNCOMMITTED` is mapped to `REPEATABLE READ`.

In PostgreSQL, `READ UNCOMMITTED` is mapped to `READ COMMITTED`.

### READ WRITE mode

Default.

### READ ONLY mode

The `READ ONLY` mode does not prevent all writes to disk.

When a transaction is `READ ONLY`, the following SQL statements are:

- Disallowed if the table they would write to is not a temporary table.
  - INSERT
  - UPDATE
  - DELETE
  - COPY FROM

- Always disallowed
  - COMMENT
  - GRANT
  - REVOKE
  - TRUNCATE

- Disallowed when the statement that would be executed is one of the above
  - EXECUTE
  - EXPLAIN ANALYZE

### DEFERRABLE mode

Use to defer a transaction only when both `SERIALIZABLE` and `READ ONLY` modes are also selected. If used, then the transaction may block when first acquiring its snapshot, after which it is able to run without the normal overhead of a `SERIALIZABLE` transaction and without any risk of contributing to, or being canceled by a serialization failure.

The `DEFERRABLE` mode may be useful for long-running reports or back-ups.

## Examples

Create a sample table.

```plpgsql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

Begin a transaction and insert some rows.

```plpgsql
yugabyte=# BEGIN TRANSACTION; SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
```

```plpgsql
yugabyte=# INSERT INTO sample(k1, k2, v1, v2) VALUES (1, 2.0, 3, 'a'), (1, 3.0, 4, 'b');
```

Start a new shell  with `ysqlsh` and begin another transaction to insert some more rows.

```plpgsql
yugabyte=# BEGIN TRANSACTION; SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
```

```plpgsql
yugabyte=# INSERT INTO sample(k1, k2, v1, v2) VALUES (2, 2.0, 3, 'a'), (2, 3.0, 4, 'b');
```

In each shell, check the only the rows from the current transaction are visible.

1st shell.

```plpgsql
yugabyte=# SELECT * FROM sample; -- run in first shell
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  1 |  3 |  4 | b
(2 rows)
```

2nd shell

```plpgsql
yugabyte=# SELECT * FROM sample; -- run in second shell
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  2 |  2 |  3 | a
  2 |  3 |  4 | b
(2 rows)
```

Commit the first transaction and abort the second one.

```plpgsql
yugabyte=# COMMIT TRANSACTION; -- run in first shell.
```

Abort the current transaction (from the first shell).

```plpgsql
yugabyte=# ABORT TRANSACTION; -- run second shell.
```

In each shell check that only the rows from the committed transaction are visible.

```plpgsql
yugabyte=# SELECT * FROM sample; -- run in first shell.
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  1 |  3 |  4 | b
(2 rows)
```

```plpgsql
yugabyte=# SELECT * FROM sample; -- run in second shell.
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  1 |  3 |  4 | b
(2 rows)
```

## See also

- [`SHOW TRANSACTION`](../txn_show)
