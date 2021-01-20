---
title: ALTER TABLE statement [YSQL]
headerTitle: ALTER TABLE
linkTitle: ALTER TABLE
description: Use the `ALTER TABLE` statement to change the definition of a table.
menu:
  latest:
    identifier: ddl_alter_table
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_alter_table/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER TABLE` statement to change the definition of a table.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_table,alter_table_action,alter_table_constraint,alter_column_constraint.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/alter_table,alter_table_action,alter_table_constraint,alter_column_constraint.diagram.md" /%}}
  </div>
</div>

## Semantics

### *alter_table*

#### ALTER TABLE [ ONLY ] *name* [ * ] [*alter\_table\_action*](#alter_table_action) [ , ... ]

Alter the specified table and dependencies.

- `ONLY` — Limit the change to the specified table.

### *alter_table_action*

Specify one of the following actions.

#### ADD [ COLUMN ] *column_name* *data_type* [*constraint*](#constraints)

Add the specified column with the specified data type and constraint.

#### RENAME TO *table_name*

Rename the table to the specified table name.

{{< note title="Note" >}}

Renaming a table is a non blocking metadata change operation.

{{< /note >}}


#### DROP [ COLUMN ] *column_name* [ RESTRICT | CASCADE ]

Drop the named column from the table. 

- `RESTRICT` — Remove only the specified

#### ADD [*alter_table_constraint*](#constraints)

Add the specified constraint to the table.

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

### Constraints

Specify a table or column constraint.

#### CONSTRAINT *constraint_name*

Specify the name of the constraint.

#### Foreign key

`FOREIGN KEY` and `REFERENCES` specify that the set of columns can only contain values that are present in the referenced columns of the referenced table. It is used to enforce referential integrity of data.

#### Unique

This enforces that the set of columns specified in the `UNIQUE` constraint are unique in the table, that is, no two rows can have the same values for the set of columns specified in the `UNIQUE` constraint.

#### Check

This is used to enforce that data in the specified table meets the requirements specified in the `CHECK` clause.

#### Default

This is used to specify a default value for the column. If an `INSERT` statement does not specify a value for the column, then the default value is used. If no default is specified for a column, then the default is NULL.

#### Deferrable constraints

Constraints can be deferred using the `DEFERRABLE` clause. Currently, only foreign key constraints
can be deferred in YugabyteDB. A constraint that is not deferrable will be checked after every row
within a statement. In the case of deferrable constraints, the checking of the constraint can be postponed
until the end of the transaction.

Constraints marked as `INITIALLY IMMEDIATE` will be checked after every row within a statement.

Constraints marked as `INITIALLY DEFERRED` will be checked at the end of the transaction.

## See also

- [`CREATE TABLE`](../ddl_create_table)
