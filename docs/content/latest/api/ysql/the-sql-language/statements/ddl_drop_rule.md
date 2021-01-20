---
title: DROP RULE statement [YSQL]
headerTitle: DROP RULE
linkTitle: DROP RULE
description: Use the DROP RULE statement to remove a rule.
menu:
  latest:
    identifier: ddl_drop_rule
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_rule/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP RULE` statement to remove a rule.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_rule.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_rule.diagram.md" /%}}
  </div>
</div>

## Semantics

See the semantics of each option in the [PostgreSQL docs][postgresql-docs-drop-rule].

## Examples

Basic example.

```plpgsql
yugabyte=# CREATE TABLE t1(a int4, b int4);
yugabyte=# CREATE TABLE t2(a int4, b int4);
yugabyte=# CREATE RULE t1_to_t2 AS ON INSERT TO t1 DO INSTEAD
             INSERT INTO t2 VALUES (new.a, new.b);
yugabyte=# DROP RULE t1_to_t2 ON t1;
```

## See also

- [`CREATE RULE`](../ddl_create_rule)
- [postgresql-docs-drop-rule](https://www.postgresql.org/docs/current/sql-droprule.html)
