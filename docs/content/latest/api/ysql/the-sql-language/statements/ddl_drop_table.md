---
title: DROP TABLE statement [YSQL]
headerTitle: DROP TABLE
linkTitle: DROP TABLE
description: Use the DROP TABLE statement to remove one or more tables (with all of their data) from the database.
menu:
  latest:
    identifier: ddl_drop_table
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_drop_table/
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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_table.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/drop_table.diagram.md" /%}}
  </div>
</div>

## Semantics

### *drop_table*

#### *if_exists*

Under normal operation, an error is raised if the table does not exist.  Adding `IF EXISTS` will quietly ignore any non-existent tables specified.

#### *table_name*

Specify the name of the table to be dropped. Objects associated with the table, such as prepared statements, will be eventually invalidated after the `DROP TABLE` statement is completed.

#### RESTRICT / CASCADE

`RESTRICT` is the default and it will not drop the procedure if any objects depend on it.

`CASCADE` will drop any objects that transitively depend on the procedure.

## Example

Do this:

```plpgsql
set client_min_messages = warning;
drop table if exists children, parents cascade;

create table parents(k int primary key, v text);

create table children(
  k int, parents_k int, v text,
  constraint children_pk primary key(k, parents_k),
  constraint children_fk foreign key(parents_k) references parents(k));
\d children
```
The `\d` metacommand output includes this information:

```
Foreign-key constraints:
    "children_fk" FOREIGN KEY (parents_k) REFERENCES parents(k)
```
Now do this:

```plpgsql
\set VERBOSITY verbose
drop table parents restrict;
```

It causes this error:

```
2BP01: cannot drop table parents because other objects depend on it
```

with this detail:

```
constraint children_fk on table children depends on table parents
```

Now do this:

```plpgsql
drop table parents cascade;
\d children
```

The 'DROP' now succeeds and the `\d` metacommand shows that the table _"children"_ still exists but that it now as no foreign key constraint to the now-dropped "_parents"_ table.

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)

```

```