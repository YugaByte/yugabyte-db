---
title: GRANT ROLE statement [YCQL]
headerTitle: GRANT ROLE
linkTitle: GRANT ROLE
description: Use the GRANT ROLE statement to grant a role's permissions and SUPERUSER status to another role.
menu:
  latest:
    parent: api-cassandra
    weight: 1282
aliases:
  - /latest/api/cassandra/ddl_grant_role
  - /latest/api/ycql/ddl_grant_role
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `GRANT ROLE` statement to grant a role's permissions and SUPERUSER status to another role. More than one role can be granted to another role, and the receiving role will possess the union of all the permissions from the roles granted to it (either directly of indirectly through inheritance) plus the SUPERUSER status if any of the roles granted to it has it. For example, if A is granted to B, and B is granted to C, C will be granted all the permissions from A and B, and if either A or B is a SUPERUSER, then C will also be a SUPERUSER.

Granted roles form an acyclic graph, in other words, a role cannot be granted to any of the roles granted to it either directly or indirectly. For example, if A is granted to B, and B granted to C, C cannot be granted to neither A, B, nor C.

This statement is enabled by setting the YB-TServer flag [`--use_cassandra_authentication`](../../../reference/configuration/yb-tserver/#config-flags) to `true`.

## Syntax

### Diagram

#### grant_role

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="305" height="35" viewbox="0 0 305 35"><path class="connector" d="M0 22h5m61 0h10m84 0h10m36 0h10m84 0h5"/><rect class="literal" x="5" y="5" width="61" height="25" rx="7"/><text class="text" x="15" y="22">GRANT</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="76" y="5" width="84" height="25"/><text class="text" x="86" y="22">role_name</text></a><rect class="literal" x="170" y="5" width="36" height="25" rx="7"/><text class="text" x="180" y="22">TO</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="216" y="5" width="84" height="25"/><text class="text" x="226" y="22">role_name</text></a></svg>

### Grammar
```
grant_role ::= GRANT ROLE role_name TO role_name
```

Where

- `role_name` is a text identifier.

## Semantics

- Both roles must exist or an error will be raised.
- Permission `AUTHORIZE` on `ALL ROLES` or on the roles being used in the statement is necessary. Otherwise, an unauthorized error will be returned.
- If a role is granted to any role granted to it (either directly or indirectly), an error will be raised.

## Examples

```sql
ycqlsh:example> GRANT ROLE eng to robert;
```

## See also

- [`ALTER ROLE`](../ddl_alter_role)
- [`DROP ROLE`](../ddl_drop_role)
- [`CREATE ROLE`](../ddl_create_role)
- [`REVOKE ROLE`](../ddl_revoke_role)
- [`GRANT PERMISSION`](../ddl_grant_permission)
- [`REVOKE PERMISSION`](../ddl_revoke_permission)
