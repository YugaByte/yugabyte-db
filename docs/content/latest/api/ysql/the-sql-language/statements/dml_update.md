---
title: UPDATE statement [YSQL]
headerTitle: UPDATE
linkTitle: UPDATE
description: Use UPDATE to modify values of specified columns in all rows that meet certain conditions. When conditions are not provided in WHERE clause, all rows update.
menu:
  latest:
    identifier: dml_update
    parent: statements
aliases:
  - /latest/api/ysql/commands/dml_update/
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `UPDATE` statement to modify the values of specified columns in all rows that meet certain conditions, and when conditions are not provided in WHERE clause, all rows are updated. `UPDATE` outputs the number of rows that are being updated.

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
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/update,returning_clause,update_item,column_values,column_names.grammar.md" /%}}
  </div>
  <div id="diagram" class="tab-pane fade" role="tabpanel" aria-labelledby="diagram-tab">
    {{% includeMarkdown "../../syntax_resources/the-sql-language/statements/update,returning_clause,update_item,column_values,column_names.diagram.md" /%}}
  </div>
</div>

See the section [The WITH clause (Common Table Expression)](../../with-clause/) for mor information about the semantics of the `with_clause_substatement_defn` grammar rule.

## Semantics

Updating columns that are part of an index key including PRIMARY KEY is not yet supported.

- While the `WHERE` clause allows a wide range of operators, the exact conditions used in the where clause have significant performance considerations (especially for large datasets). For the best performance, use a `WHERE` clause that provides values for all columns in `PRIMARY KEY` or `INDEX KEY`.

### *with_query*

Specify the subqueries that are referenced by name in the `UPDATE` statement.

### *table_name*

Specify the name of the table to be updated.

### *alias*

Specify the identifier of the target table within the `UPDATE` statement. When an alias is specified, it must be used in place of the actual table in the statement.

### *column_name*

Specify the column in the table to be updated.

### *expression*

Specify the value to be assigned to a column. When the expression is referencing a column, the old value of this column is used to evaluate.

### *output_expression*

Specify the value to be returned. When the `output_expression` is referencing a column, the new value of this column (updated value) is used to evaluate.

### *subquery*

Specify the SELECT subquery statement. Its selected values will be assigned to the specified columns.

## Examples

Create a sample table, insert a few rows, then update the inserted rows.

```plpgsql
yugabyte=# CREATE TABLE sample(k1 int, k2 int, v1 int, v2 text, PRIMARY KEY (k1, k2));
```

```plpgsql
yugabyte=# INSERT INTO sample VALUES (1, 2.0, 3, 'a'), (2, 3.0, 4, 'b'), (3, 4.0, 5, 'c');
```

```plpgsql
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

```plpgsql
yugabyte=# UPDATE sample SET v1 = v1 + 3, v2 = '7' WHERE k1 = 2 AND k2 = 3;
```

```
UPDATE 1
```

```plpgsql
yugabyte=# SELECT * FROM sample ORDER BY k1;
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

- [`DELETE`](../dml_delete)
- [`INSERT`](../dml_insert)
- [`SELECT`](../dml_select)
