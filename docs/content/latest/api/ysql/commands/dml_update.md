---
title: UPDATE
linkTitle: UPDATE
summary: Update table data
description: UPDATE
menu:
  latest:
    identifier: api-ysql-commands-update
    parent: api-ysql-commands
aliases:
  - /latest/api/ysql/commands/dml_update
isTocNested: true
showAsideToc: true
---

## Synopsis

UPDATE modifies the values of specified columns in all rows that meet certain conditions, and when conditions are not provided in WHERE clause, all rows are updated. UPDATE outputs the number of rows that are being updated.

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
    {{% includeMarkdown "../syntax_resources/commands/update,returning_clause,update_item,column_values,column_names.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../syntax_resources/commands/update,returning_clause,update_item,column_values,column_names.diagram.md" /%}}
  </div>
</div>

Where

- `with_query` specifies the subqueries that are referenced by name in the UPDATE statement.

- `table_name` specifies a name of the table to be updated.

- `alias` is the identifier of the target table within the UPDATE statement. When an alias is specified, it must be used in place of the actual table in the statement.

- `column_name` specifies a column in the table to be updated.

- `expression` specifies the value to be assigned a column. When the expression is referencing a column, the old value of this column is used to evaluate.

- `output_expression` specifies the value to be returned. When the `output_expression` is referencing a column, the new value of this column (updated value) is used to evaluate.

- `subquery` is a SELECT statement. Its selected values will be assigned to the specified columns.

## Semantics

- Updating columns that are part of an index key including PRIMARY KEY is not yet supported.

- While the where clause allows a wide range of operators, the exact conditions used in the where clause have significant performance considerations (especially for large datasets). WHERE clause that provides values for all columns in PRIMARY KEY or INDEX KEY has the best performance.

## Examples

Create a sample table, insert a few rows, then update the inserted rows.

```sql
postgres=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

```sql
postgres=# INSERT INTO sample VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
```

```sql
postgres=# SELECT * FROM sample ORDER BY k1;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  2 |  3 |  4 | b
  3 |  4 |  5 | c
(3 rows)
```

```sql
postgres=# UPDATE sample SET v1 = v1 + 3, v2 = '7' WHERE k1 = 2 AND k2 = 3;
```

```
UPDATE 1
```

```sql
postgres=# SELECT * FROM sample ORDER BY k1;
```

```
 k1 | k2 | v1 | v2
----+----+----+----
  1 |  2 |  3 | a
  2 |  3 |  7 | 7
  3 |  4 |  5 | c
(2 rows)
```

## See also

[`DELETE`](../dml_delete)
[`INSERT`](../dml_insert)
[`SELECT`](../dml_select)
[Other YSQL Statements](..)
