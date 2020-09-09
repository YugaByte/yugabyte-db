---
title: SELECT statement [YCQL]
headerTitle: SELECT
linkTitle: SELECT
summary: Retrieves rows from a table
description: Use the SELECT statement to retrieve (part of) rows of specified columns that meet a given condition from a table.
block_indexing: true
menu:
  v2.1:
    parent: api-cassandra
    weight: 1310
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `SELECT` statement to retrieve (part of) rows of specified columns that meet a given condition from a table. It specifies the columns to be retrieved, the name of the table, and the condition each selected row must satisfy.

## Syntax

### Diagram

#### select

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="593" height="310" viewbox="0 0 593 310"><path class="connector" d="M0 22h15m67 0h30m79 0h20m-114 0q5 0 5 5v8q0 5 5 5h89q5 0 5-5v-8q0-5 5-5m5 0h30m28 0h139m-182 0q5 0 5 5v50q0 5 5 5h25m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h47q5 0 5 5v20q0 5-5 5m-5 0h25q5 0 5-5v-50q0-5 5-5m5 0h10m55 0h10m95 0h7m2 0h2m2 0h2m-593 95h2m2 0h2m2 0h27m66 0h10m137 0h30m134 0h20m-169 0q5 0 5 5v8q0 5 5 5h144q5 0 5-5v-8q0-5 5-5m5 0h20m-432 0q5 0 5 5v23q0 5 5 5h407q5 0 5-5v-23q0-5 5-5m5 0h7m2 0h2m2 0h2m-467 65h2m2 0h2m2 0h27m32 0h10m104 0h20m-181 0q5 0 5 5v8q0 5 5 5h156q5 0 5-5v-8q0-5 5-5m5 0h7m2 0h2m2 0h2m-216 50h2m2 0h2m2 0h27m82 0h10m131 0h20m-258 0q5 0 5 5v8q0 5 5 5h233q5 0 5-5v-8q0-5 5-5m5 0h7m2 0h2m2 0h2m-293 50h2m2 0h2m2 0h27m54 0h10m122 0h20m-221 0q5 0 5 5v8q0 5 5 5h196q5 0 5-5v-8q0-5 5-5m5 0h30m67 0h10m133 0h20m-245 0q5 0 5 5v8q0 5 5 5h220q5 0 5-5v-8q0-5 5-5m5 0h15"/><polygon points="0,29 5,22 0,15" style="fill:black;stroke-width:0"/><rect class="literal" x="15" y="5" width="67" height="25" rx="7"/><text class="text" x="25" y="22">SELECT</text><rect class="literal" x="112" y="5" width="79" height="25" rx="7"/><text class="text" x="122" y="22">DISTINCT</text><rect class="literal" x="241" y="5" width="28" height="25" rx="7"/><text class="text" x="251" y="22">*</text><rect class="literal" x="302" y="35" width="24" height="25" rx="7"/><text class="text" x="312" y="52">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="261" y="65" width="107" height="25"/><text class="text" x="271" y="82">column_name</text></a><rect class="literal" x="418" y="5" width="55" height="25" rx="7"/><text class="text" x="428" y="22">FROM</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="483" y="5" width="95" height="25"/><text class="text" x="493" y="22">table_name</text></a><rect class="literal" x="35" y="100" width="66" height="25" rx="7"/><text class="text" x="45" y="117">WHERE</text><a xlink:href="../grammar_diagrams#where-expression"><rect class="rule" x="111" y="100" width="137" height="25"/><text class="text" x="121" y="117">where_expression</text></a><rect class="literal" x="278" y="100" width="134" height="25" rx="7"/><text class="text" x="288" y="117">ALLOW FILTERING</text><rect class="literal" x="35" y="165" width="32" height="25" rx="7"/><text class="text" x="45" y="182">IF</text><a xlink:href="../grammar_diagrams#if-expression"><rect class="rule" x="77" y="165" width="104" height="25"/><text class="text" x="87" y="182">if_expression</text></a><rect class="literal" x="35" y="215" width="82" height="25" rx="7"/><text class="text" x="45" y="232">ORDER BY</text><a xlink:href="../grammar_diagrams#order-expression"><rect class="rule" x="127" y="215" width="131" height="25"/><text class="text" x="137" y="232">order_expression</text></a><rect class="literal" x="35" y="265" width="54" height="25" rx="7"/><text class="text" x="45" y="282">LIMIT</text><a xlink:href="../grammar_diagrams#limit-expression"><rect class="rule" x="99" y="265" width="122" height="25"/><text class="text" x="109" y="282">limit_expression</text></a><rect class="literal" x="271" y="265" width="67" height="25" rx="7"/><text class="text" x="281" y="282">OFFSET</text><a xlink:href="../grammar_diagrams#offset-expression"><rect class="rule" x="348" y="265" width="133" height="25"/><text class="text" x="358" y="282">offset_expression</text></a><polygon points="512,289 516,289 516,275 512,275" style="fill:black;stroke-width:0"/></svg>

#### order_expression

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="351" height="100" viewbox="0 0 351 100"><path class="connector" d="M0 52h15m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h98m24 0h99q5 0 5 5v20q0 5-5 5m-109 0h30m45 0h29m-84 25q0 5 5 5h5m54 0h5q5 0 5-5m-79-25q5 0 5 5v33q0 5 5 5h64q5 0 5-5v-33q0-5 5-5m5 0h30m25 0h15"/><polygon points="0,59 5,52 0,45" style="fill:black;stroke-width:0"/><rect class="literal" x="15" y="35" width="25" height="25" rx="7"/><text class="text" x="25" y="52">(</text><rect class="literal" x="163" y="5" width="24" height="25" rx="7"/><text class="text" x="173" y="22">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="70" y="35" width="107" height="25"/><text class="text" x="80" y="52">column_name</text></a><rect class="literal" x="207" y="35" width="45" height="25" rx="7"/><text class="text" x="217" y="52">ASC</text><rect class="literal" x="207" y="65" width="54" height="25" rx="7"/><text class="text" x="217" y="82">DESC</text><rect class="literal" x="311" y="35" width="25" height="25" rx="7"/><text class="text" x="321" y="52">)</text><polygon points="347,59 351,59 351,45 347,45" style="fill:black;stroke-width:0"/></svg>

### Grammar

```
select ::= SELECT [ DISTINCT ] { * | column_name [ , column_name ... ] } 
               FROM table_name
               [ WHERE where_expression ]
               [ IF where_expression ]
               [ ORDER BY order_expression ]
               [ LIMIT limit_expression ] [ OFFSET offset_expression ]

order_expression ::= ( { column_name [ ASC | DESC ] } [ , ... ] )
```

Where

- `table_name` and `column_name` are identifiers (`table_name` may be qualified with a keyspace name).
- `limit_expression` is an integer literal (or a bind variable marker for prepared statements).
- Restrictions for `where_expression` are discussed in the Semantics section below.
- See [Expressions](..#expressions) for more information on syntax rules.

## Semantics

- An error is raised if the specified `table_name` does not exist.
- `SELECT DISTINCT` can only be used for partition columns or static columns.
- `*` means all columns of the table will be retrieved.
- `LIMIT` clause sets the maximum number of results (rows) to be returned.
- `OFFSET` clause sets the number of rows to be skipped before returning results.

### `ORDER BY` clause

- The `ORDER BY` clause sets the order for the returned results.
- Only clustering columns are allowed in the `order_expression`.
- For a given column, `DESC` means descending order and `ASC` or omitted means ascending order.
- Currently, only two overall orderings are allowed, the clustering order from the `CREATE TABLE` statement (forward scan) or its opposite (reverse scan).

### `WHERE` clause

- The `where_expression` must evaluate to boolean values.
- The `where_expression` can specify conditions on any columns including partition, clustering, and regular columns.
- The `where_expression` has a restricted list of operators.

  - Only `=`, `IN` and `NOT IN` operators can be used for conditions on partition columns. 
  - Only operators `=`, `<`, `<=`, `>`, `>=`, `IN` and `NOT IN` can be used for conditions on clustering and regular columns.

### `IF` clause

- The `if_expression` must evaluate to boolean values.
- The `if_expression` supports any combinations of all available boolean and logical operators.
- The `if_expression` can only specify conditions for non-primary-key columns although it can used on a key column of a secondary index.
- While WHERE condition is used to generate efficient query plan, the IF condition is not. ALL rows that satisfy WHERE condition will be read from the database before the IF condition is used to filter unwanted data. In the following example, although the two queries yield the same result set, SELECT with WHERE clause will use INDEX-SCAN while SELECT with IF clause will use FULL-SCAN.

```
SELECT * FROM a_table WHERE key = 'my_key';
SELECT * FROM a_table IF key = 'my_key';
```

{{< note title="Note" >}}
While the where clause allows a wide range of operators, the exact conditions used in the where clause have significant performance considerations (especially for large datasets).
Some best practices are:

- Use equality conditions on all partition columns (to fix the value of the partition key).
- Use comparison operators on the clustering columns (tighter restrictions are more valuable for left-most clustering columns).
- Generally, the closer a column is to the beginning of the primary key, the higher the performance gain for setting tighter restrictions on it. 

Ideally, these performance considerations should be taken into account when creating the table schema.{{< /note >}}

## Examples

### Select all rows from a table

```sql
ycqlsh:example> CREATE TABLE employees(department_id INT, 
                                      employee_id INT, 
                                      dept_name TEXT STATIC,
                                      employee_name TEXT, 
                                      PRIMARY KEY(department_id, employee_id));
```

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, dept_name, employee_name) 
                   VALUES (1, 1, 'Accounting', 'John');
```

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, dept_name, employee_name) 
                   VALUES (1, 2, 'Accounting', 'Jane');
```

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, dept_name, employee_name) 
                   VALUES (1, 3, 'Accounting', 'John');
```

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, dept_name, employee_name) 
                   VALUES (2, 1, 'Marketing', 'Joe');
```

```sql
ycqlsh:example> SELECT * FROM employees;
```

```
 department_id | employee_id | dept_name  | employee_name
---------------+-------------+------------+---------------
             1 |           1 | Accounting |          John
             1 |           2 | Accounting |          Jane
             1 |           3 | Accounting |          John
             2 |           1 |  Marketing |           Joe
```

### Select with limit

```sql
ycqlsh:example> SELECT * FROM employees LIMIT 2;
```

```
 department_id | employee_id | dept_name  | employee_name
---------------+-------------+------------+---------------
             1 |           1 | Accounting |          John
             1 |           2 | Accounting |          Jane
```

### Select with offset

```sql
ycqlsh:example> SELECT * FROM employees LIMIT 2 OFFSET 1;
```

```
 department_id | employee_id | dept_name  | employee_name
---------------+-------------+------------+---------------
             1 |           2 | Accounting |          Jane
             1 |           3 | Accounting |          John
```

### Select distinct values

```sql
ycqlsh:example> SELECT DISTINCT dept_name FROM employees;
```

```
 dept_name
------------
 Accounting
  Marketing
```

### Select with a condition on the partitioning column

```sql
ycqlsh:example> SELECT * FROM employees WHERE department_id = 2;
```

```
 department_id | employee_id | dept_name | employee_name
---------------+-------------+-----------+---------------
             2 |           1 | Marketing |           Joe
```

### Select with condition on the clustering column

```sql
ycqlsh:example> SELECT * FROM employees WHERE department_id = 1 AND employee_id <= 2;
```

```
 department_id | employee_id | dept_name  | employee_name
---------------+-------------+------------+---------------
             1 |           1 | Accounting |          John
             1 |           2 | Accounting |          Jane
```

### Select with condition on a regular column, using WHERE clause

```sql
ycqlsh:example> SELECT * FROM employees WHERE department_id = 1 AND employee_name = 'John' ALLOW FILTERING;
```

```
 department_id | employee_id | dept_name  | employee_name
---------------+-------------+------------+---------------
             1 |           1 | Accounting |          John
             1 |           3 | Accounting |          John
```

### Select with condition on a regular column, using IF clause

```sql
ycqlsh:example> SELECT * FROM employees WHERE department_id = 1 IF employee_name != 'John';
```

```
 department_id | employee_id | dept_name  | employee_name
---------------+-------------+------------+---------------
             1 |           2 | Accounting |          Jane
```

### Select with `ORDER BY` clause

``` sql
ycqlsh:example> CREATE TABLE sensor_data(device_id INT,
                                        sensor_id INT,
                                        ts TIMESTAMP,
                                        value TEXT,
                                        PRIMARY KEY((device_id), sensor_id, ts)) WITH CLUSTERING ORDER BY (sensor_id ASC, ts DESC);
```

```sql
ycqlsh:example> INSERT INTO sensor_data(device_id, sensor_id, ts, value)
                   VALUES (1, 1, '2018-1-1 12:30:30 UTC', 'a');
```

```sql
ycqlsh:example> INSERT INTO sensor_data(device_id, sensor_id, ts, value)
                   VALUES (1, 1, '2018-1-1 12:30:31 UTC', 'b');
```

```sql
ycqlsh:example> INSERT INTO sensor_data(device_id, sensor_id, ts, value)
                   VALUES (1, 2, '2018-1-1 12:30:30 UTC', 'x');
```

```sql
ycqlsh:example> INSERT INTO sensor_data(device_id, sensor_id, ts, value)
                   VALUES (1, 2, '2018-1-1 12:30:31 UTC', 'y');
```

Reverse scan, opposite of the table's clustering order.

```sql
ycqlsh:example> SELECT * FROM sensor_data WHERE device_id = 1 ORDER BY sensor_id DESC, ts ASC;
```

```
 device_id | sensor_id | ts                              | value
-----------+-----------+---------------------------------+-------
         1 |         2 | 2018-01-01 12:30:30.000000+0000 |     x
         1 |         2 | 2018-01-01 12:30:31.000000+0000 |     y
         1 |         1 | 2018-01-01 12:30:30.000000+0000 |     a
         1 |         1 | 2018-01-01 12:30:31.000000+0000 |     b
```

Forward scan, same as a SELECT without an ORDER BY clause.

```sql
ycqlsh:example> SELECT * FROM sensor_data WHERE device_id = 1 ORDER BY sensor_id ASC, ts DESC;
```

```
 device_id | sensor_id | ts                              | value
-----------+-----------+---------------------------------+-------
         1 |         1 | 2018-01-01 12:30:31.000000+0000 |     b
         1 |         1 | 2018-01-01 12:30:30.000000+0000 |     a
         1 |         2 | 2018-01-01 12:30:31.000000+0000 |     y
         1 |         2 | 2018-01-01 12:30:30.000000+0000 |     x
```

Other orderings are not allowed.

```sql
ycqlsh:example> SELECT * FROM sensor_data WHERE device_id = 1 ORDER BY sensor_id ASC, ts ASC;
```

```
InvalidRequest: Unsupported order by relation
SELECT * FROM sensor_data WHERE device_id = 1 ORDER BY sensor_id ASC, ts ASC;
                                                        ^^^^^^^^^^^^^^^^^^^^^
```

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`INSERT`](../dml_insert)
- [`UPDATE`](../dml_update)
- [`DELETE`](../dml_delete)
- [`Expression`](..#expressions)
