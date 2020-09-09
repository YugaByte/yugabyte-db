---
title: TRUNCATE
headerTitle: TRUNCATE
linkTitle: TRUNCATE
description:  Use the TRUNCATE statement to clear all rows in a table.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-truncate
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `TRUNCATE` statement to clear all rows in a table.

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
    {{% includeMarkdown "../syntax_resources/commands/truncate.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/truncate.diagram.md" /%}}
  </div>
</div>

## Semantics

### *truncate*

#### TRUNCATE [ TABLE ] { { [ ONLY ] *name* [ * ] } [ , ... ] }

#### *name*

Specify the name of the table to be truncated.

- `TRUNCATE` acquires `ACCESS EXCLUSIVE` lock on the tables to be truncated. The `ACCESS EXCLUSIVE` locking option is not yet fully supported.
- `TRUNCATE` is not supported for foreign tables.

## Examples

```postgresql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

```postgresql
yugabyte=# INSERT INTO sample VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
```

```postgresql
yugabyte=# SELECT * FROM sample ORDER BY k1;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  2 |  3 |  4 | b
  3 |  4 |  5 | c
(3 rows)
```

```postgresql
yugabyte=# TRUNCATE sample;
```

```postgresql
yugabyte=# SELECT * FROM sample;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
(0 rows)
```
