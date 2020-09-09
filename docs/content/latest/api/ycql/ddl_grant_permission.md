---
title: GRANT PERMISSION statement [YCQL]
headerTitle: GRANT PERMISSION
linkTitle: GRANT PERMISSION
description: Use the GRANT PERMISSION statement to grant a permission (or all the available permissions) to a role.
menu:
  latest:
    parent: api-cassandra
    weight: 1281
aliases:
  - /latest/api/cassandra/ddl_grant_permission
  - /latest/api/ycql/ddl_grant_permission
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `GRANT PERMISSION` statement to grant a permission (or all the available permissions) to a role.

When a database object is created (keyspace, table, or role), an automatic and explicit grant of all the permissions relevant to the object are granted to the role creating it.

This statement is enabled by setting the YB-TServer flag [`--use_cassandra_authentication`](../../../reference/configuration/yb-tserver/#config-flags) to `true`.

## Syntax

### Diagram

#### grant_permission

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="501" height="65" viewbox="0 0 501 65"><path class="connector" d="M0 22h5m61 0h30m111 0h20m-146 0q5 0 5 5v20q0 5 5 5h5m84 0h32q5 0 5-5v-20q0-5 5-5m5 0h10m38 0h10m71 0h10m36 0h10m84 0h5"/><rect class="literal" x="5" y="5" width="61" height="25" rx="7"/><text class="text" x="15" y="22">GRANT</text><a xlink:href="../grammar_diagrams#all-permissions"><rect class="rule" x="96" y="5" width="111" height="25"/><text class="text" x="106" y="22">all_permissions</text></a><a xlink:href="../grammar_diagrams#permission"><rect class="rule" x="96" y="35" width="84" height="25"/><text class="text" x="106" y="52">permission</text></a><rect class="literal" x="237" y="5" width="38" height="25" rx="7"/><text class="text" x="247" y="22">ON</text><a xlink:href="../grammar_diagrams#resource"><rect class="rule" x="285" y="5" width="71" height="25"/><text class="text" x="295" y="22">resource</text></a><rect class="literal" x="366" y="5" width="36" height="25" rx="7"/><text class="text" x="376" y="22">TO</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="412" y="5" width="84" height="25"/><text class="text" x="422" y="22">role_name</text></a></svg>

#### all_permissions

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="207" height="50" viewbox="0 0 207 50"><path class="connector" d="M0 22h5m42 0h30m105 0h20m-140 0q5 0 5 5v8q0 5 5 5h115q5 0 5-5v-8q0-5 5-5m5 0h5"/><rect class="literal" x="5" y="5" width="42" height="25" rx="7"/><text class="text" x="15" y="22">ALL</text><rect class="literal" x="77" y="5" width="105" height="25" rx="7"/><text class="text" x="87" y="22">PERMISSIONS</text></svg>

#### permission

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="288" height="245" viewbox="0 0 288 245"><path class="connector" d="M0 22h25m67 0h44m-121 25q0 5 5 5h5m58 0h38q5 0 5-5m-111 30q0 5 5 5h5m53 0h43q5 0 5-5m-111 30q0 5 5 5h5m66 0h30q5 0 5-5m-111 30q0 5 5 5h5m67 0h29q5 0 5-5m-111 30q0 5 5 5h5m91 0h5q5 0 5-5m-111 30q0 5 5 5h5m82 0h14q5 0 5-5m-116-175q5 0 5 5v200q0 5 5 5h5m76 0h20q5 0 5-5v-200q0-5 5-5m5 0h30m97 0h20m-132 0q5 0 5 5v8q0 5 5 5h107q5 0 5-5v-8q0-5 5-5m5 0h5"/><rect class="literal" x="25" y="5" width="67" height="25" rx="7"/><text class="text" x="35" y="22">CREATE</text><rect class="literal" x="25" y="35" width="58" height="25" rx="7"/><text class="text" x="35" y="52">ALTER</text><rect class="literal" x="25" y="65" width="53" height="25" rx="7"/><text class="text" x="35" y="82">DROP</text><rect class="literal" x="25" y="95" width="66" height="25" rx="7"/><text class="text" x="35" y="112">SELECT</text><rect class="literal" x="25" y="125" width="67" height="25" rx="7"/><text class="text" x="35" y="142">MODIFY</text><rect class="literal" x="25" y="155" width="91" height="25" rx="7"/><text class="text" x="35" y="172">AUTHORIZE</text><rect class="literal" x="25" y="185" width="82" height="25" rx="7"/><text class="text" x="35" y="202">DESCRIBE</text><rect class="literal" x="25" y="215" width="76" height="25" rx="7"/><text class="text" x="35" y="232">EXECUTE</text><rect class="literal" x="166" y="5" width="97" height="25" rx="7"/><text class="text" x="176" y="22">PERMISSION</text></svg>

#### resource

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="258" height="170" viewbox="0 0 258 170"><path class="connector" d="M0 22h25m42 0h30m90 0h20m-125 0q5 0 5 5v20q0 5 5 5h5m60 0h35q5 0 5-5v-20q0-5 5-5m5 0h46m-238 55q0 5 5 5h5m82 0h10m116 0h5q5 0 5-5m-228 30q0 5 5 5h25m58 0h20m-93 0q5 0 5 5v8q0 5 5 5h68q5 0 5-5v-8q0-5 5-5m5 0h10m91 0h14q5 0 5-5m-233-85q5 0 5 5v125q0 5 5 5h5m52 0h10m84 0h67q5 0 5-5v-125q0-5 5-5m5 0h5"/><rect class="literal" x="25" y="5" width="42" height="25" rx="7"/><text class="text" x="35" y="22">ALL</text><rect class="literal" x="97" y="5" width="90" height="25" rx="7"/><text class="text" x="107" y="22">KEYSPACES</text><rect class="literal" x="97" y="35" width="60" height="25" rx="7"/><text class="text" x="107" y="52">ROLES</text><rect class="literal" x="25" y="65" width="82" height="25" rx="7"/><text class="text" x="35" y="82">KEYSPACE</text><a xlink:href="../grammar_diagrams#keyspace-name"><rect class="rule" x="117" y="65" width="116" height="25"/><text class="text" x="127" y="82">keyspace_name</text></a><rect class="literal" x="45" y="95" width="58" height="25" rx="7"/><text class="text" x="55" y="112">TABLE</text><a xlink:href="../grammar_diagrams#table-name"><rect class="rule" x="133" y="95" width="91" height="25"/><text class="text" x="143" y="112">table_name</text></a><rect class="literal" x="25" y="140" width="52" height="25" rx="7"/><text class="text" x="35" y="157">ROLE</text><a xlink:href="../grammar_diagrams#role-name"><rect class="rule" x="87" y="140" width="84" height="25"/><text class="text" x="97" y="157">role_name</text></a></svg>

### Grammar

```
grant_permission := GRANT all_permission | permission ON resource TO role_name;
all_permissions := ALL [ PERMISSIONS ]
permission :=  ( CREATE | ALTER | DROP | SELECT | MODIFY | AUTHORIZE | DESCRIBE | EXECUTE ) [ PERMISSION ]
resource := ALL ( KEYSPACES | ROLES ) | KEYSPACE keyspace_name | [ TABLE ] table_name | ROLE role_name;
```

Where

- `keyspace_name`, `table_name`, and `role_name` are text identifiers (`table_name` may be qualified with a keyspace name).

## Semantics

- Permission `AUTHORIZE` on `ALL ROLES` or on the role being used in the statement is necessary. Otherwise, an unauthorized error will be returned.

## Permissions

This section describes the permissions (represented by `ALTER`, `AUTHORIZE`, `CREATE`, `DESCRIBE`, `DROP`, `MODIFY`, and `SELECT`) that are necessary to execute operations on the database objects. A permission can be granted on a specific object (represented by resources `KEYSPACE`, `TABLE`, and `ROLE`) or on a whole group of objects (represented by resources `ALL KEYSPACES`, and `ALL ROLES`). Some permissions are granted implicitly, which means that you will never see them listed when you query `system_auth.role_permissions` table. Implicitly granted permissions follow these rules:

- Any permission granted on `ALL KEYSPACES` is implicitly granted on every keyspace and table in the database.
- Any permission granted on a specific `KEYSPACE` is implicitly granted to any table in that keyspace.
- Any permission granted on `ALL ROLES` is implicitly granted on every role.

### Permissions needed to execute specific operations on a database object

Operation| Permission| Resource|
---------|-----------|---------|
`ALTER KEYSPACE`| `ALTER`| `ALL KEYSPACES`, or `KEYSPACE`
`ALTER ROLE`| `ALTER`| `ALL ROLES`, or `ROLE`
`ALTER TABLE`| `ALTER`| `ALL KEYSPACES`, `KEYSPACE`, or `TABLE`
`CREATE KEYSPACE`| `CREATE`| `ALL KEYSPACES`
`CREATE ROLE`| `CREATE`| `ALL ROLES`
`CREATE TABLE`| `CREATE`| `ALL KEYSPACES`, `KEYSPACE`
`DROP KEYSPACE`| `DROP`| `ALL KEYSPACES`, or `KEYSPACE`
`DROP ROLE`| `DROP`| `ALL ROLES`, or `ROLE`
`DROP TABLE`| `DROP`| `ALL KEYSPACES`, `KEYSPACE`, or `TABLE`
`GRANT PERMISSION` or `REVOKE PERMISSION` on `ALL KEYSPACES`| `AUTHORIZE`| `ALL KEYSPACES`
`GRANT PERMISSION` or `REVOKE PERMISSION` on `ALL ROLES`| `AUTHORIZE`| `ALL ROLES`
`GRANT PERMISSION` or `REVOKE PERMISSION` on a keyspace| `AUTHORIZE`| `ALL KEYSPACES`, or `KEYSPACE`
`GRANT PERMISSION` or `REVOKE PERMISSION` on a role| `AUTHORIZE` | `ALL ROLES`, or `ROLE`
`GRANT PERMISSION` or `REVOKE PERMISSION` on a table| `AUTHORIZE`| `ALL KEYSPACES`, `KEYSPACE`, or `TABLE`
`GRANT ROLE` or `REVOKE ROLE`| `AUTHORIZE` | `ALL ROLES`, or `ROLE`
`INSERT`, `UPDATE`, `DELETE`, or `TRUNCATE`| `MODIFY`| `ALL KEYSPACES`, `KEYSPACE`, or `TABLE`
`LIST ROLES` (not yet implemented)| `DESCRIBE`| `ALL ROLES`
`SELECT`| `SELECT`| `ALL KEYSPACES`, `KEYSPACE`, or `TABLE`

## Examples

### Grant `MODIFY` permission on a table so role `qa` can insert rows into a table.

```sql
ycqlsh:example> GRANT MODIFY ON TABLE performance_tests.metrics TO qa;
```

### Grant `SELECT` permission on a table so role `qa` can read the table.

```sql
ycqlsh:example> GRANT SELECT ON performance_tests.metrics TO qa;
```

### Grant `CREATE` permission on `ALL KEYSPACES` so role `tests` can create new keyspaces.

```sql
ycqlsh:example> GRANT CREATE ON ALL KEYSPACES TO tests;
```

## See also

- [`ALTER ROLE`](../ddl_alter_role)
- [`DROP ROLE`](../ddl_drop_role)
- [`CREATE ROLE`](../ddl_create_role)
- [`REVOKE ROLE`](../ddl_revoke_role)
- [`GRANT PERMISSION`](../ddl_grant_permission)
- [`REVOKE PERMISSION`](../ddl_revoke_permission)
