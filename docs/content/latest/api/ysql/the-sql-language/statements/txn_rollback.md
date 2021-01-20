---
title: ROLLBACK statement [YSQL]
headerTitle: ROLLBACK
linkTitle: ROLLBACK
description: Use the ROLLBACK statement to roll back the current transactions.
menu:
  latest:
    identifier: txn_rollback
    parent: statements
aliases:
  - /latest/api/ysql/commands/txn_rollback/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ROLLBACK` statement to roll back the current transactions. All changes included in this transactions will be discarded.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/rollback.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/rollback.diagram.md" /%}}
  </div>
</div>

## Semantics

### *rollback*

```
ROLLBACK [ TRANSACTION | WORK ]
```

### WORK

Add optional keyword — has no effect.

### TRANSACTION

Add optional keyword — has no effect.

## See also

- [`BEGIN`](../txn_begin)
- [`COMMIT`](../txn_commit)
