---
title: ALTER KEYSPACE statement [YCQL]
headerTitle: ALTER KEYSPACE
linkTitle: ALTER KEYSPACE
description: Use the ALTER KEYSPACE statement to change the properties of an existing keyspace.
menu:
  latest:
    parent: api-cassandra
    weight: 1200
aliases:
  - /latest/api/cassandra/ddl_alter_keyspace
  - /latest/api/ycql/ddl_alter_keyspace
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `ALTER KEYSPACE` statement to change the properties of an existing keyspace.

This statement is supported for compatibility reasons only, and has no effect internally (no-op statement).

The statement can fail if the specified keyspace does not exist or if the user (role) has no permissions for the keyspace ALTER operation.

## Syntax

### Diagram

#### alter_keyspace

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="471" height="65" viewbox="0 0 471 65"><path class="connector" d="M0 22h5m58 0h30m82 0h20m-117 0q5 0 5 5v20q0 5 5 5h5m71 0h16q5 0 5-5v-20q0-5 5-5m5 0h10m110 0h10m141 0h5"/><rect class="literal" x="5" y="5" width="58" height="25" rx="7"/><text class="text" x="15" y="22">ALTER</text><rect class="literal" x="93" y="5" width="82" height="25" rx="7"/><text class="text" x="103" y="22">KEYSPACE</text><rect class="literal" x="93" y="35" width="71" height="25" rx="7"/><text class="text" x="103" y="52">SCHEMA</text><a xlink:href="../grammar_diagrams#keyspace-name"><rect class="rule" x="205" y="5" width="110" height="25"/><text class="text" x="215" y="22">keyspace_name</text></a><a xlink:href="../grammar_diagrams#keyspace-properties"><rect class="rule" x="325" y="5" width="141" height="25"/><text class="text" x="335" y="22">keyspace_properties</text></a></svg>

#### keyspace_properties

<svg class="rrdiagram" version="1.1" xmlns:xlink="http://www.w3.org/1999/xlink" xmlns="http://www.w3.org/2000/svg" width="888" height="110" viewbox="0 0 888 110"><path class="connector" d="M0 52h25m53 0h10m101 0h10m30 0h10m28 0h30m-5 0q-5 0-5-5v-20q0-5 5-5h59m24 0h59q5 0 5 5v20q0 5-5 5m-5 0h30m28 0h20m-497 0q5 0 5 5v8q0 5 5 5h472q5 0 5-5v-8q0-5 5-5m5 0h30m46 0h10m133 0h10m30 0h30m45 0h22m-82 0q5 0 5 5v20q0 5 5 5h5m47 0h5q5 0 5-5v-20q0-5 5-5m5 0h20m-361 0q5 0 5 5v38q0 5 5 5h336q5 0 5-5v-38q0-5 5-5m5 0h5"/><rect class="literal" x="25" y="35" width="53" height="25" rx="7"/><text class="text" x="35" y="52">WITH</text><rect class="literal" x="88" y="35" width="101" height="25" rx="7"/><text class="text" x="98" y="52">REPLICATION</text><rect class="literal" x="199" y="35" width="30" height="25" rx="7"/><text class="text" x="209" y="52">=</text><rect class="literal" x="239" y="35" width="28" height="25" rx="7"/><text class="text" x="249" y="52">{</text><rect class="literal" x="351" y="5" width="24" height="25" rx="7"/><text class="text" x="361" y="22">,</text><a xlink:href="../grammar_diagrams#keyspace-property"><rect class="rule" x="297" y="35" width="132" height="25"/><text class="text" x="307" y="52">keyspace_property</text></a><rect class="literal" x="459" y="35" width="28" height="25" rx="7"/><text class="text" x="469" y="52">}</text><rect class="literal" x="537" y="35" width="46" height="25" rx="7"/><text class="text" x="547" y="52">AND</text><rect class="literal" x="593" y="35" width="133" height="25" rx="7"/><text class="text" x="603" y="52">DURABLE_WRITES</text><rect class="literal" x="736" y="35" width="30" height="25" rx="7"/><text class="text" x="746" y="52">=</text><rect class="literal" x="796" y="35" width="45" height="25" rx="7"/><text class="text" x="806" y="52">true</text><rect class="literal" x="796" y="65" width="47" height="25" rx="7"/><text class="text" x="806" y="82">false</text></svg>

### Grammar

```
alter_keyspace ::= ALTER { KEYSPACE | SCHEMA } keyspace_name
                       [ WITH REPLICATION '=' '{' keyspace_property '}']
                       [ AND DURABLE_WRITES '=' { true | false } ]

keyspace_property ::= property_name = property_value
```

Where

- `keyspace_name` and `property_name` are identifiers.
- `property_value` is a literal of either [boolean](../type_bool), [text](../type_text), or [map](../type_collection) data type.

## Semantics

- An error is raised if the specified `keyspace_name` does not exist.
- An error is raised if the user (used role) has no ALTER permission for this specified keyspace and no ALTER permission for ALL KEYSPACES.
- YCQL keyspace properties are supported in the syntax but have no effect internally (where YugabyteDB defaults are used instead).

## Examples

```sql
ycqlsh> ALTER KEYSPACE example;
```

```sql
ycqlsh> ALTER KEYSPACE example WITH DURABLE_WRITES = true;
```

```sql
ycqlsh> ALTER KEYSPACE example WITH REPLICATION = {'class': 'SimpleStrategy', 'replication_factor': '3'} AND DURABLE_WRITES = true;
```

```sql
ycqlsh> ALTER SCHEMA keyspace_example;
```

```
SQL error: Keyspace Not Found.
ALTER SCHEMA keyspace_example;
             ^^^^^^
```

```sql
ycqlsh> ALTER KEYSPACE example;
```

```
SQL error: Unauthorized. User test_role has no ALTER permission on <keyspace example> or any of its parents.
ALTER KEYSPACE example;
^^^^^^
```

## See also

- [`CREATE KEYSPACE`](../ddl_create_keyspace)
- [`DROP KEYSPACE`](../ddl_drop_keyspace)
- [`USE`](../ddl_use)
