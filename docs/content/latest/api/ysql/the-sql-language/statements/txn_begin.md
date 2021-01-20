---
title: BEGIN statement [YSQL]
headerTitle: BEGIN
linkTitle: BEGIN
description: Use the `BEGIN` statement to start a transaction with the default (or given) isolation level.
menu:
  latest:
    identifier: txn_begin
    parent: statements
aliases:
  - /latest/api/ysql/commands/txn_begin/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `BEGIN` statement to start a transaction with the default (or given) isolation level.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/begin.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/begin.diagram.md" /%}}
  </div>
</div>

## Semantics

### *begin*

```plpgsql
BEGIN [ TRANSACTION | WORK ] [ transaction_mode [ ... ] ]
```

#### WORK

Add optional keyword — has no effect.

#### TRANSACTION

Add optional keyword — has no effect.

### *transaction_mode*

Supports both Serializable and Snapshot Isolation using the PostgreSQL isolation level syntax of `SERIALIZABLE` and `REPEATABLE READ` respectively. Even `READ COMMITTED` and `READ UNCOMMITTED` isolation levels are mapped to Snapshot Isolation.

## Examples

Create a sample table.

```plpgsql
CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

Begin a transaction and insert some rows.

```plpgsql
BEGIN TRANSACTION; SET TRANSACTION ISOLATION LEVEL REPEATABLE READ; 
```

```plpgsql
INSERT INTO sample(k1, k2, v1, v2) VALUES (1, 2.0, 3, 'a'), (1, 3.0, 4, 'b');
```

Start a new shell  with `ysqlsh` and begin another transaction to insert some more rows.

```plpgsql
BEGIN TRANSACTION; SET TRANSACTION ISOLATION LEVEL REPEATABLE READ; 
```

```plpgsql
INSERT INTO sample(k1, k2, v1, v2) VALUES (2, 2.0, 3, 'a'), (2, 3.0, 4, 'b');
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
COMMIT TRANSACTION; -- run in first shell.
```

Abort the current transaction (from the first shell).

```plpgsql
ABORT TRANSACTION; -- run second shell.
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

- [`ABORT`](../txn_abort)
- [`COMMIT`](../txn_commit)
- [`END`](../txn_end)
- [`ROLLBACK`](../txn_rollback)
- [`SET TRANSACTION`](../txn_set)
