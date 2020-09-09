---
title: ALTER TABLE statement [YSQL]
headerTitle: ALTER TABLE
linkTitle: ALTER TABLE
description: Use the `ALTER TABLE` statement to change the definition of an existing table.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands-alter-table
    parent: api-ysql-commands
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER TABLE` statement to change the definition of an existing table.

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
    {{% includeMarkdown "../syntax_resources/commands/alter_table,alter_table_action,alter_table_constraint.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/alter_table,alter_table_action,alter_table_constraint.diagram.md" /%}}
  </div>
</div>

## Semantics

### *alter_table*

#### ALTER TABLE [ ONLY ] *name* [ * ] [*alter\_table\_action*](#alter_table_action) [ , ... ]

Alter the specified table and dependencies.

- `ONLY` — Limit the change to the specified table.

### *alter_table_action*

Specify one of the following actions.

#### ADD [ COLUMN ] *column_name* *data_type*

Add the specified column with the specified data type.

#### RENAME TO *table_name*

Rename the table to the specified table name.

#### DROP [ COLUMN ] *column_name* [ RESTRICT | CASCADE ]

Drop the named column from the table. 

- `RESTRICT` — Remove only the specified

#### ADD [*alter_table_constraint*](#alter-table-constraint)

Add the specified constraint to the table. For descriptions of valid *table_constraint* values, see [CREATE TABLE](../ddl_create_table).

#### DROP CONSTRAINT *constraint_name* [ RESTRICT | CASCADE ]

Drop the named constraint from the table.

- `RESTRICT` — Remove only the specified constraint.
- `CASCADE` — Remove the specified constraint and any dependencies.

#### RENAME [ COLUMN ] *column_name* TO *column_name*

Rename a column to the specified name.

#### ENABLE / DISABLE ROW LEVEL SECURITY

This enables or disables row level security for the table.
If enabled and no policies exist for the table, then a default-deny policy is applied.
If disabled, then existing policies for the table will not be applied and will be ignored.
See [CREATE POLICY](../dcl_create_policy) for details on how to create row level security policies.

#### FORCE / NO FORCE ROW LEVEL SECURITY

This controls the application of row security policies for the table when the user is the table owner.
If enabled, row level security policies will be applied when the user is the table owner.
If disabled (the default) then row level security will not be applied when the user is the table owner.
See [CREATE POLICY](../dcl_create_policy) for details on how to create row level security policies.

### *alter_table_constraint*

Specify a table constraint.

#### CONSTRAINT *constraint_name*

Specify the name of the constraint.

#### CHECK ( expression ) | FOREIGN KEY ( column_names ) *reference_clause*

## See also

- [`CREATE TABLE`](../ddl_create_table)
