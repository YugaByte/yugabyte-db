---
title: COMMENT statement [YSQL]
headerTitle: COMMENT
linkTitle: COMMENT
description: Use the COMMENT statement to set, update, or remove a comment on a database object.
menu:
  latest:
    identifier: ddl_comment
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_comment/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `COMMENT` statement to set, update, or remove a comment on a database object.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/comment_on.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/comment_on.diagram.md" /%}}
  </div>
</div>

## Semantics

To remove a comment, set the value to `NULL`.

### *comment_on*

#### COMMENT ON

Add or change a comment about a database object. To remove a comment, set the value to `NULL`.

### *aggregate_signature*

## Examples

### Add a comment

```plpgsql
COMMENT ON DATABASE postgres IS 'Default database';
```

```plpgsql
COMMENT ON INDEX index_name IS 'Special index';
```

### Remove a comment

```plpgsql
COMMENT ON TABLE some_table IS NULL;
```