---
title: CREATE TABLE AS statement [YSQL]
headerTitle: CREATE TABLE AS
linkTitle: CREATE TABLE AS
description: Use the CREATE TABLE AS statement to create a table using the output of a subquery.
menu:
  latest:
    identifier: ddl_create_table_as
    parent: statements
aliases:
  - /latest/api/ysql/commands/ddl_create_table_as/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE TABLE AS` statement to create a table using the output of a subquery.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_table_as.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/create_table_as.diagram.md" /%}}
  </div>
</div>

## Semantics

YugabyteDB may extend the syntax to allow specifying PRIMARY KEY for `CREATE TABLE AS` command.

### *create_table_as*

#### CREATE TABLE [ IF NOT EXISTS ] *table_name*

Create a table.

##### *table_name*

Specify the name of the table.

##### ( *column_name* [ , ... ] )

Specify the name of a column in the new table. When not specified, column names are taken from the output column names of the query.

#### AS *query* [ WITH [ NO ] DATA ]

##### *query*

## Examples

```plpgsql
CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

```plpgsql
INSERT INTO sample VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
```

```plpgsql
CREATE TABLE selective_sample SELECT * FROM sample WHERE k1 > 1;
```

```plpgsql
yugabyte=# SELECT * FROM selective_sample ORDER BY k1;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  2 |  3 |  4 | b
  3 |  4 |  5 | c
(2 rows)
```

## See also

- [`CREATE TABLE`](../ddl_create_table)
