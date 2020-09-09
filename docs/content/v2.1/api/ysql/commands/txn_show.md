---
title: SHOW TRANSACTION statement [YSQL]
headerTitle: SHOW TRANSACTION
linkTitle: SHOW TRANSACTION
description: Use the SHOW TRANSACTION statement to show the current transaction isolation level.
summary: SHOW TRANSACTION
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-txn-show
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SHOW TRANSACTION` statement to show the current transaction isolation level.

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
    {{% includeMarkdown "../syntax_resources/commands/show_transaction.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/show_transaction.diagram.md" /%}}
  </div>
</div>

## Semantics

Supports both Serializable and Snapshot Isolation using the PostgreSQL isolation level syntax of `SERIALIZABLE` and `REPEATABLE READS` respectively. Even `READ COMMITTED` and `READ UNCOMMITTED` isolation levels are mapped to Snapshot Isolation.

### TRANSACTION ISOLATION LEVEL

Show the current transaction isolation level.

The `TRANSACTION ISOLATION LEVEL` returned is either `SERIALIZABLE` or `REPEATABLE READS`. In YugabyteDB, the `READ COMMITTED` and `READ UNCOMMITTED` of PostgreSQL are mapped to `REPEATABLE READS`.

## See also

- [`SHOW TRANSACTION`](../txn_show)
- [`Transaction isolation levels`](../../../../architecture/transactions/isolation-levels)
