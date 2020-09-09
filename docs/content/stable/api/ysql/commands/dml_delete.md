---
title: DELETE statement [YSQL]
headerTitle: DELETE
linkTitle: DELETE
description: Use the DELETE statement to remove rows that meet certain conditions, and when conditions are not provided in WHERE clause, all rows are deleted.
block_indexing: true
menu:
  stable:
    identifier: api-ysql-commands-delete
    parent: api-ysql-commands
aliases:
  - /stable/api/ysql/commands/dml_delete
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DELETE` statement to remove rows that meet certain conditions, and when conditions are not provided in WHERE clause, all rows are deleted. `DELETE` outputs the number of rows that are being deleted.

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
    {{% includeMarkdown "../syntax_resources/commands/delete,returning_clause.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/delete,returning_clause.diagram.md" /%}}
  </div>
</div>

## Semantics

- `USING` clause is not yet supported.

- While the `WHERE` clause allows a wide range of operators, the exact conditions used in the `WHERE` clause have significant performance considerations (especially for large datasets). For the best performance, use a `WHERE` clause that provides values for all columns in `PRIMARY KEY` or `INDEX KEY`.

### *delete* 

#### WITH [ RECURSIVE ] *with_query* [ , ... ] DELETE FROM [ ONLY ] *table_name* [ * ] [ [ AS ] *alias* ] [ WHERE *condition* | WHERE CURRENT OF *cursor_name* ] [ [*returning_clause*] (#returning-clause) ]

##### *with_query*

Specify the subqueries that are referenced by name in the DELETE statement.

##### *table_name*

Specify the name of the table to be deleted.

##### *alias*

Specify the identifier of the target table within the DELETE statement. When an alias is specified, it must be used in place of the actual table in the statement.

### *returning_clause*

#### RETURNING

Specify the value to be returned. When the _output_expression_ references a column, the existing value of this column (deleted value) is used to evaluate.

#### *output_name*

## Examples

Create a sample table, insert a few rows, then delete one of the inserted row.

```postgresql
CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

```postgresql
INSERT INTO sample VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
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
DELETE FROM sample WHERE k1 = 2 AND k2 = 3;
```

```postgresql
yugabyte=# SELECT * FROM sample ORDER BY k1;
```

```
DELETE 1
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  3 |  4 |  5 | c
(2 rows)
```

## See also

- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
- [`UPDATE`](../dml_update)
