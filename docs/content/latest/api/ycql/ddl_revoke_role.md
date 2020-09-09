---
title: REVOKE ROLE statement [YCQL]
headerTitle: REVOKE ROLE
linkTitle: REVOKE ROLE
description: Use the `REVOKE ROLE` statement to revoke a role (which represents a group of permissions and the SUPERUSER status) from another role.
menu:
  latest:
    parent: api-cassandra
    weight: 1284
aliases:
  - /latest/api/cassandra/ddl_revoke_role
  - /latest/api/ycql/ddl_revoke_role
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `REVOKE ROLE` statement to revoke a role (which represents a group of permissions and the SUPERUSER status) from another role.

This statement is enabled by setting the YB-TServer flag [`--use_cassandra_authentication`](../../../reference/configuration/yb-tserver/#config-flags) to `true`.

## Syntax

### Diagram

#### revoke_role

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="331" height="35" viewbox="0 0 331 35"><path class="connector" d="M0 22h5m69 0h10m84 0h10m54 0h10m84 0h5"/><rect class="literal" x="5" y="5" width="69" height="25" rx="7"/><text class="text" x="15" y="22">REVOKE</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="84" y="5" width="84" height="25"/><text class="text" x="94" y="22">role_name</text></a><rect class="literal" x="178" y="5" width="54" height="25" rx="7"/><text class="text" x="188" y="22">FROM</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="242" y="5" width="84" height="25"/><text class="text" x="252" y="22">role_name</text></a></svg>

### Grammar

```
revoke_role ::= REVOKE ROLE role_name FROM role_name
```

Where

- `role_name` is a text identifier.

## Semantics

- Both roles must exist or an error will be raised.
- Permission `AUTHORIZE` on `ALL ROLES` or on the roles being used in the statement is necessary. Otherwise, an unauthorized error will be returned.
- You cannot revoke a role that hasn't been granted or an error will be raised.

## Examples

```sql
ycqlsh:example> REVOKE ROLE project_y from diana;
```

## See also

- [`ALTER ROLE`](../ddl_alter_role)
- [`DROP ROLE`](../ddl_drop_role)
- [`CREATE ROLE`](../ddl_create_role)
- [`GRANT ROLE`](../ddl_grant_role)
- [`GRANT PERMISSION`](../ddl_grant_permission)
- [`REVOKE PERMISSION`](../ddl_revoke_permission)
