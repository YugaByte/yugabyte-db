---
title: ALTER ROLE statement [YCQL]
headerTitle: ALTER ROLE
linkTitle: ALTER ROLE
description: Use the ALTER ROLE statement to change the properties of an existing role.
menu:
  latest:
    parent: api-cassandra
    weight: 1210
aliases:
  - /latest/api/cassandra/ddl_alter_role
  - /latest/api/ycql/ddl_alter_role
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER ROLE` statement to change the properties of an existing role.
It allows modifying properties `SUPERUSER`, `PASSWORD`, and `LOGIN`.

This statement is enabled by setting the YB-TServer flag [`--use_cassandra_authentication`](../../../reference/configuration/yb-tserver/#use-cassandra-authentication) to `true`.

## Syntax

### Diagram

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="437" height="65" viewbox="0 0 437 65"><path class="connector" d="M0 52h5m58 0h10m52 0h10m84 0h10m53 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h32m46 0h32q5 0 5 5v20q0 5-5 5m-5 0h25"/><rect class="literal" x="5" y="35" width="58" height="25" rx="7"/><text class="text" x="15" y="52">ALTER</text><rect class="literal" x="73" y="35" width="52" height="25" rx="7"/><text class="text" x="83" y="52">ROLE</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="135" y="35" width="84" height="25"/><text class="text" x="145" y="52">role_name</text></a><rect class="literal" x="229" y="35" width="53" height="25" rx="7"/><text class="text" x="239" y="52">WITH</text><rect class="literal" x="339" y="5" width="46" height="25" rx="7"/><text class="text" x="349" y="22">AND</text><a xlink:href="../grammar_diagrams#role-property"><rect class="rule" x="312" y="35" width="100" height="25"/><text class="text" x="322" y="52">role_property</text></a></svg>

### Grammar
```
alter_table ::= ALTER ROLE role_name WITH role_property [ AND role_property ...];

role_property ::=  PASSWORD = '<Text Literal>'
                 | LOGIN = <Boolean Literal>'
                 | SUPERUSER = '<Boolean Literal>'
```

Where

- `role_name` is a text identifier.

## Semantics

An error is raised if `role_name` does not exist.

## Examples

You can do this as shown below.

```sql
ycqlsh:example> CREATE ROLE finance;
```

```sql
ycqlsh:example> ALTER ROLE finance with LOGIN = true;
```

```sql
ycqlsh:example> ALTER ROLE finance with SUPERUSER = true;
```

```sql
ycqlsh:example> ALTER ROLE finance with PASSWORD = 'jsfp9ajhufans2' AND SUPERUSER = false;
```

## See also

- [`CREATE ROLE`](../ddl_create_role)
- [`DROP ROLE`](../ddl_drop_role)
- [`GRANT ROLE`](../ddl_grant_role)
- [`REVOKE ROLE`](../ddl_revoke_role)
- [`GRANT PERMISSION`](../ddl_grant_permission)
- [`REVOKE PERMISSION`](../ddl_revoke_permission)
