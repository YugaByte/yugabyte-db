---
title: DROP TABLE statement [YSQL]
headerTitle: DROP TABLE
linkTitle: DROP TABLE
description: Use the DROP TABLE statement to remove one or more tables (with all of their data) from the database.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-drop-table
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP TABLE` statement to remove one or more tables (with all of their data) from the database.

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
    {{% includeMarkdown "../syntax_resources/commands/drop_table.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/drop_table.diagram.md" /%}}
  </div>
</div>

## Semantics

### *drop_table*

#### *if_exists*

Under normal operation, an error is raised if the table does not exist.  Adding `IF EXISTS` will quietly ignore any non-existent tables specified.

#### *table_name*

Specify the name of the table to be dropped. Objects associated with the table, such as prepared statements, will be eventually invalidated after the `DROP TABLE` statement is completed.

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
