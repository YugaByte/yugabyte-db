---
title: CREATE TYPE statement [YCQL]
headerTitle: CREATE TYPE
linkTitle: CREATE TYPE
description: Use the CREATE TYPE statement to create a new user-defined data type in a keyspace.
menu:
  latest:
    parent: api-cassandra
    weight: 1250
aliases:
  - /latest/api/cassandra/ddl_create_type
  - /latest/api/ycql/ddl_create_type
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `CREATE TYPE` statement to create a new user-defined data type in a keyspace.  It defines the name of the user-defined type and the names and data types for its fields.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="739" height="80" viewbox="0 0 739 80"><path class="connector" d="M0 52h5m67 0h10m49 0h30m32 0h10m45 0h10m64 0h20m-196 0q5 0 5 5v8q0 5 5 5h171q5 0 5-5v-8q0-5 5-5m5 0h10m88 0h10m25 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h80m24 0h80q5 0 5 5v20q0 5-5 5m-93 0h10m78 0h30m25 0h5"/><rect class="literal" x="5" y="35" width="67" height="25" rx="7"/><text class="text" x="15" y="52">CREATE</text><rect class="literal" x="82" y="35" width="49" height="25" rx="7"/><text class="text" x="92" y="52">TYPE</text><rect class="literal" x="161" y="35" width="32" height="25" rx="7"/><text class="text" x="171" y="52">IF</text><rect class="literal" x="203" y="35" width="45" height="25" rx="7"/><text class="text" x="213" y="52">NOT</text><rect class="literal" x="258" y="35" width="64" height="25" rx="7"/><text class="text" x="268" y="52">EXISTS</text><a xlink:href="../grammar_diagrams#type-name"><rect class="rule" x="352" y="35" width="88" height="25"/><text class="text" x="362" y="52">type_name</text></a><rect class="literal" x="450" y="35" width="25" height="25" rx="7"/><text class="text" x="460" y="52">(</text><rect class="literal" x="580" y="5" width="24" height="25" rx="7"/><text class="text" x="590" y="22">,</text><a xlink:href="../grammar_diagrams#field-name"><rect class="rule" x="505" y="35" width="86" height="25"/><text class="text" x="515" y="52">field_name</text></a><a xlink:href="../grammar_diagrams#field-type"><rect class="rule" x="601" y="35" width="78" height="25"/><text class="text" x="611" y="52">field_type</text></a><rect class="literal" x="709" y="35" width="25" height="25" rx="7"/><text class="text" x="719" y="52">)</text></svg>

### Grammar

```
create_type ::= CREATE TYPE [ IF NOT EXISTS ] type_name
                    (field_name field_type [ ',' field_name field_type ...]);
```

Where

- `type_name` and `field_name` are identifiers (`type_name` may be qualified with a keyspace name).
- `field_type` is a data type.

## Semantics

- An error is raised if the specified `type_name` already exists in the associated keyspace unless the `IF NOT EXISTS` option is used.
- Each `field_name` must each be unique (a type cannot have two fields of the same name).
- Each `field_type` must be either a [non-parametric type](../#data-types) or a [frozen type](../type_frozen).

## Examples

Collection types must be frozen to be used inside a user-defined type.

```sql
ycqlsh:example> CREATE TYPE person(first_name TEXT, last_name TEXT, emails FROZEN<LIST<TEXT>>);
```

```sql
ycqlsh:example> DESCRIBE TYPE person;
```

```
CREATE TYPE example.person (
    first_name text,
    last_name text,
    emails frozen<list<text>>
);
```

```sql
ycqlsh:example> CREATE TABLE employees(employee_id INT PRIMARY KEY, employee person);
```

```sql
ycqlsh:example> INSERT INTO employees(employee_id, employee)
                   VALUES (1, {first_name : 'John', last_name : 'Doe', emails : ['jdoe@example.com']});
```

```sql
ycqlsh:example> SELECT * FROM employees;
```

```
 employee_id | employee
-------------+---------------------------------------------------------------------------
           1 | {first_name: 'John', last_name: 'Doe', emails: ['john.doe@yugabyte.com']}

```

## See also

- [`CREATE TABLE`](../ddl_create_table)
- [`DROP TYPE`](../ddl_drop_keyspace)
