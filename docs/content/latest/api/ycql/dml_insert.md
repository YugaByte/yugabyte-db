---
title: INSERT statement [YCQL]
headerTitle: INSERT
linkTitle: INSERT
description: Use the INSERT statement to add a row to a specified table.
menu:
  latest:
    parent: api-cassandra
    weight: 1300
aliases:
  - /latest/api/cassandra/dml_insert
  - /latest/api/ycql/dml_insert
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `INSERT` statement to add a row to a specified table.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="785" height="160" viewbox="0 0 785 160"><path class="connector" d="M0 52h15m65 0h10m50 0h10m91 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h46m24 0h46q5 0 5 5v20q0 5-5 5m-5 0h30m25 0h10m68 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h34m24 0h35q5 0 5 5v20q0 5-5 5m-5 0h30m25 0h7m2 0h2m2 0h2m-763 35h2m2 0h2m2 0h27m32 0h50m45 0h20m-80 0q5 0 5 5v8q0 5 5 5h55q5 0 5-5v-8q0-5 5-5m5 0h10m64 0h20m-194 0q5 0 5 5v35q0 5 5 5h5m98 0h66q5 0 5-5v-35q0-5 5-5m5 0h20m-276 0q5 0 5 5v53q0 5 5 5h251q5 0 5-5v-53q0-5 5-5m5 0h30m60 0h10m123 0h20m-228 0q5 0 5 5v8q0 5 5 5h203q5 0 5-5v-8q0-5 5-5m5 0h30m181 0h20m-216 0q5 0 5 5v8q0 5 5 5h191q5 0 5-5v-8q0-5 5-5m5 0h15"/><polygon points="0,59 5,52 0,45" style="fill:black;stroke-width:0"/><rect class="literal" x="15" y="35" width="65" height="25" rx="7"/><text class="text" x="25" y="52">INSERT</text><rect class="literal" x="90" y="35" width="50" height="25" rx="7"/><text class="text" x="100" y="52">INTO</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="150" y="35" width="91" height="25"/><text class="text" x="160" y="52">table_name</text></a><rect class="literal" x="251" y="35" width="25" height="25" rx="7"/><text class="text" x="261" y="52">(</text><rect class="literal" x="347" y="5" width="24" height="25" rx="7"/><text class="text" x="357" y="22">,</text><a xlink:href="../grammar_diagrams#column-name"><rect class="rule" x="306" y="35" width="106" height="25"/><text class="text" x="316" y="52">column_name</text></a><rect class="literal" x="442" y="35" width="25" height="25" rx="7"/><text class="text" x="452" y="52">)</text><rect class="literal" x="477" y="35" width="68" height="25" rx="7"/><text class="text" x="487" y="52">VALUES</text><rect class="literal" x="555" y="35" width="25" height="25" rx="7"/><text class="text" x="565" y="52">(</text><rect class="literal" x="639" y="5" width="24" height="25" rx="7"/><text class="text" x="649" y="22">,</text><a xlink:href="../grammar_diagrams#expression"><rect class="rule" x="610" y="35" width="83" height="25"/><text class="text" x="620" y="52">expression</text></a><rect class="literal" x="723" y="35" width="25" height="25" rx="7"/><text class="text" x="733" y="52">)</text><rect class="literal" x="35" y="70" width="32" height="25" rx="7"/><text class="text" x="45" y="87">IF</text><rect class="literal" x="117" y="70" width="45" height="25" rx="7"/><text class="text" x="127" y="87">NOT</text><rect class="literal" x="192" y="70" width="64" height="25" rx="7"/><text class="text" x="202" y="87">EXISTS</text><a xlink:href="../grammar_diagrams#if-expression"><rect class="rule" x="97" y="115" width="98" height="25"/><text class="text" x="107" y="132">if_expression</text></a><rect class="literal" x="326" y="70" width="60" height="25" rx="7"/><text class="text" x="336" y="87">USING</text><a xlink:href="../grammar_diagrams#using-expression"><rect class="rule" x="396" y="70" width="123" height="25"/><text class="text" x="406" y="87">using_expression</text></a><rect class="literal" x="569" y="70" width="181" height="25" rx="7"/><text class="text" x="579" y="87">RETURNS STATUS AS ROW</text><polygon points="781,94 785,94 785,80 781,80" style="fill:black;stroke-width:0"/></svg>

### using_expression

```
using_expression = ttl_or_timestamp_expression { 'AND' ttl_or_timestamp_expression };
```

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="246" height="65" viewbox="0 0 246 65"><path class="connector" d="M0 52h25m-5 0q-5 0-5-5v-20q0-5 5-5h80m46 0h80q5 0 5 5v20q0 5-5 5m-5 0h25"/><rect class="literal" x="100" y="5" width="46" height="25" rx="7"/><text class="text" x="110" y="22">AND</text><a xlink:href="../grammar_diagrams#ttl-or-timestamp-expression"><rect class="rule" x="25" y="35" width="196" height="25"/><text class="text" x="35" y="52">ttl_or_timestamp_expression</text></a></svg>

### ttl_or_timestamp_expression

```
ttl_or_timestamp_expression = 'TTL' ttl_expression | 'TIMESTAMP' timestamp_expression;
```

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="305" height="65" viewbox="0 0 305 65"><path class="connector" d="M0 22h25m41 0h10m104 0h120m-290 0q5 0 5 5v20q0 5 5 5h5m90 0h10m155 0h5q5 0 5-5v-20q0-5 5-5m5 0h5"/><rect class="literal" x="25" y="5" width="41" height="25" rx="7"/><text class="text" x="35" y="22">TTL</text><a xlink:href="../grammar_diagrams#ttl-expression"><rect class="rule" x="76" y="5" width="104" height="25"/><text class="text" x="86" y="22">ttl_expression</text></a><rect class="literal" x="25" y="35" width="90" height="25" rx="7"/><text class="text" x="35" y="52">TIMESTAMP</text><a xlink:href="../grammar_diagrams#timestamp-expression"><rect class="rule" x="125" y="35" width="155" height="25"/><text class="text" x="135" y="52">timestamp_expression</text></a></svg>

### Grammar

```
insert ::= INSERT INTO table_name ( column_name [ , ... ] ) VALUES ( 
           expression [ , ... ] )  
           [ IF { [ NOT ] EXISTS | if_expression } ] 
           [ USING using_expression ] 
           [ RETURNS STATUS AS ROW ]
```

Where

- `table_name` and `column` are identifiers (`table_name` may be qualified with a keyspace name).
- `value` can be any expression although Apache Cassandra requires that `value`s must be literals.
- Restrictions for `if_expression` and `ttl_expression` are covered in the Semantics section below.
- See [Expressions](..#expressions) for more information on syntax rules.

## Semantics

- An error is raised if the specified `table_name` does not exist. 
- The columns list must include all primary key columns.
- The `USING TIMESTAMP` clause indicates you would like to perform the INSERT as if it was done at the
  timestamp provided by the user. The timestamp is the number of microseconds since epoch.
- By default `INSERT` has `upsert` semantics, that is, if the row already exists, it behaves like an `UPDATE`. If pure
 `INSERT` semantics is desired then the `IF NOT EXISTS` clause can be used to make sure an existing row is not overwritten by the `INSERT`.
- **NOTE**: You should either use the `USING TIMESTAMP` clause in all of your statements or none of
  them. Using a mix of statements where some have `USING TIMESTAMP` and others do not will lead to
  very confusing results.

### `VALUES` clause

- The values list must have the same length as the columns list.
- Each value must be convertible to its corresponding (by position) column type.
- Each value literal can be an expression that evaluates to a simple value.

### `IF` clause

- The `if_expression` can only apply to non-key columns (regular columns).
- The `if_expression` can contain any logical and boolean operators.

### `USING` clause

- `ttl_expression` must be an integer value (or a bind variable marker for prepared statements).
- `timestamp_expression` must be an integer value (or a bind variable marker for prepared statements).

## Examples

### Insert a row into a table

```sql
ycqlsh:example> CREATE TABLE employees(department_id INT,
                                      employee_id INT,
                                      name TEXT,
                                      PRIMARY KEY(department_id, employee_id));
```

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, name) VALUES (1, 1, 'John');
```

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, name) VALUES (1, 2, 'Jane');
```

```sql
ycqlsh:example> SELECT * FROM employees;
```

```
 department_id | employee_id | name
---------------+-------------+------
             1 |           1 | John
             1 |           2 | Jane
```

### Conditional insert using the `IF` clause

Example 1

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, name) VALUES (2, 1, 'Joe') IF name = null;
```

```
 [applied]
-----------
      True
```

Example 2

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, name) VALUES (2, 1, 'Jack') IF NOT EXISTS;
```

```
 [applied]
-----------
     False
```

Example 3

```sql
ycqlsh:example> SELECT * FROM employees;
```

```
 department_id | employee_id | name
---------------+-------------+------
             2 |           1 |  Joe
             1 |           1 | John
             1 |           2 | Jane
```

### Insert a row with expiration time using the `USING TTL` clause

You can do this as shown below.

```sql
ycqlsh:example> INSERT INTO employees(department_id, employee_id, name) VALUES (2, 2, 'Jack') USING TTL 10;
```

Now query the employees table.

```sql
ycqlsh:example> SELECT * FROM employees;
```

```
 department_id | employee_id | name
---------------+-------------+------
             2 |           1 |  Joe
             2 |           2 | Jack
             1 |           1 | John
             1 |           2 | Jane
```

Again query the employees table after 11 seconds or more.

```sql
ycqlsh:example> SELECT * FROM employees; -- 11 seconds after the insert.
```

```
 department_id | employee_id | name
---------------+-------------+------
             2 |           1 |  Joe
             1 |           1 | John
             1 |           2 | Jane
```

### Insert a row with `USING TIMESTAMP` clause

#### Insert a row with a low timestamp

```sql
ycqlsh:foo> INSERT INTO employees(department_id, employee_id, name) VALUES (1, 3, 'Jeff') USING TIMESTAMP 1000;
```

Now query the employees table.

```sql
ycqlsh:foo> SELECT * FROM employees;
```

```
 department_id | employee_id | name
---------------+-------------+------
             1 |           1 | John
             1 |           2 | Jane
             1 |           3 | Jeff
             2 |           1 |  Joe

(4 rows)
```

#### Overwrite the row with a higher timestamp

```sql
ycqlsh:foo> INSERT INTO employees(department_id, employee_id, name) VALUES (1, 3, 'Jerry') USING TIMESTAMP 2000;
```

```sql
ycqlsh:foo> SELECT * FROM employees;
```

```
 department_id | employee_id | name
---------------+-------------+-------
             1 |           1 |  John
             1 |           2 |  Jane
             1 |           3 | Jerry
             2 |           1 |   Joe

(4 rows)
```

#### Try to overwrite the row with a lower timestamp

```sql
ycqlsh:foo> INSERT INTO employees(department_id, employee_id, name) VALUES (1, 3, 'James') USING TIMESTAMP 1500;
```

```sql
ycqlsh:foo> SELECT * FROM employees;
```

```
 department_id | employee_id | name
---------------+-------------+-------
             1 |           1 |  John
             1 |           2 |  Jane
             1 |           3 | Jerry
             2 |           1 |   Joe

(4 rows)
```

### RETURNS STATUS AS ROW

When executing a batch in YCQL, the protocol returns only one error or return status. The `RETURNS STATUS AS ROW` feature addresses this limitation and adds a status row for each statement.

See examples in [batch docs](../batch#row-status).



## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`DELETE`](../dml_delete)
- [`SELECT`](../dml_select)
- [`UPDATE`](../dml_update)
- [`Expression`](..#expressions)
