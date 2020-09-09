---
title: DROP ROLE statement [YCQL]
headerTitle: DROP ROLE
linkTitle: DROP ROLE
description: Use the DROP ROLE statement to delete an existing role.
menu:
  latest:
    parent: api-cassandra
    weight: 1265
aliases:
  - /latest/api/cassandra/ddl_drop_role
  - /latest/api/ycql/ddl_drop_role
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `DROP ROLE` statement to delete an existing role.

This statement is enabled by setting the YB-TServer flag [`use_cassandra_authentication`](../../../reference/configuration/yb-tserver/#use-cassandra-authentication) to `true`.

## Syntax

### Diagram

### drop_role

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="375" height="50" viewbox="0 0 375 50"><path class="connector" d="M0 22h5m53 0h10m52 0h30m32 0h10m64 0h20m-141 0q5 0 5 5v8q0 5 5 5h116q5 0 5-5v-8q0-5 5-5m5 0h10m84 0h5"/><rect class="literal" x="5" y="5" width="53" height="25" rx="7"/><text class="text" x="15" y="22">DROP</text><rect class="literal" x="68" y="5" width="52" height="25" rx="7"/><text class="text" x="78" y="22">ROLE</text><rect class="literal" x="150" y="5" width="32" height="25" rx="7"/><text class="text" x="160" y="22">IF</text><rect class="literal" x="192" y="5" width="64" height="25" rx="7"/><text class="text" x="202" y="22">EXISTS</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="286" y="5" width="84" height="25"/><text class="text" x="296" y="22">role_name</text></a></svg>

### Grammar

```
drop_role ::=  DROP ROLE [ IF EXISTS ] role_name
```

Where

- `role_name` is a text identifier.

## Semantics

- An error is raised if `role_name` does not exist unless IF EXISTS option is present.
- Only a role with the `SUPERUSER` status can delete another `SUPERUSER` role.
- Only a client with the permission `DROP` on `ALL ROLES` or on the specified `role_name`, or with the `SUPERUSER` status can delete another role.

## Examples

You can do this as shown below.

```sql
ycqlsh:example> DROP ROLE role1;
```

```sql
ycqlsh:example> DROP ROLE IF EXISTS role2;
```

## See also

- [`ALTER ROLE`](../ddl_alter_role)
- [`CREATE ROLE`](../ddl_drop_role)
- [`GRANT ROLE`](../ddl_grant_role)
- [`REVOKE ROLE`](../ddl_revoke_role)
- [`GRANT PERMISSION`](../ddl_grant_permission)
- [`REVOKE PERMISSION`](../ddl_revoke_permission)
