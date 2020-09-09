---
title: Yugabyte Cloud Query Language (YCQL)
headerTitle: Yugabyte Cloud Query Language (YCQL)
linkTitle: YCQL
description: YCQL is a semi-relational API that is best fit for internet-scale OLTP & HTAP applications.
summary: Reference for the YCQL API
image: /images/section_icons/api/ycql.png
headcontent:
menu:
  latest:
    identifier: api-cassandra
    parent: api
    weight: 3000
aliases:
  - /latest/api/ycql/
  - /latest/api/cassandra/
isTocNested: false
showAsideToc: true
---

## Introduction

Yugabyte Cloud Query Language (YCQL) is a semi-relational SQL API that is best fit for internet-scale OLTP and HTAP applications needing massive data ingestion and blazing-fast queries. It supports strongly consistent secondary indexes, a native JSON column type, and distributed transactions. It has its roots in the [Cassandra Query Language (CQL)](http://cassandra.apache.org/doc/latest/cql/index.html). 

This page covers the following YCQL features.

- Data definition language (DDL) statements.
- Data manipulation language (DML) statements.
- Builtin functions and Expression operators.
- Primitive user-defined data types.

## Quick Start

You can explore the basics of the YCQL API using the [Quick start](quick-start/) steps.

## DDL statements

Data definition language (DDL) statements are instructions for the following database operations.

- Create, alter, and drop database objects
- Create, grant, and revoke users and roles

Statement | Description |
----------|-------------|
[`ALTER TABLE`](ddl_alter_table) | Alter a table |
[`ALTER KEYSPACE`](ddl_alter_keyspace) | Alter a keyspace |
[`CREATE INDEX`](ddl_create_index) | Create a new index on a table |
[`CREATE KEYSPACE`](ddl_create_keyspace) | Create a new keyspace |
[`CREATE TABLE`](ddl_create_table) | Create a new table |
[`CREATE TYPE`](ddl_create_type) | Create a user-defined data type |
[`DROP INDEX`](ddl_drop_index) | Remove an index |
[`DROP KEYSPACE`](ddl_drop_keyspace) | Remove a keyspace |
[`DROP TABLE`](ddl_drop_table) | Remove a table |
[`DROP TYPE`](ddl_drop_type) | Remove a user-defined data type |
[`USE`](ddl_use) | Use an existing keyspace for subsequent commands |

## DDL security statements

Security statements are instructions for managing and restricting operations on the database objects.

This feature is enabled by setting the YB-TServer configuration flag [`--use_cassandra_authentication`](../../reference/configuration/yb-tserver/#use-cassandra-authentication) to `true`.

- Create, grant, and revoke users and roles
- Grant, and revoke permissions on database objects

Statement | Description |
----------|-------------|
[`ALTER ROLE`](ddl_alter_role) | Alter a role |
[`CREATE ROLE`](ddl_create_role) | Create a new role |
[`DROP ROLE`](ddl_drop_role) | Remove a role |
[`GRANT PERMISSION`](ddl_grant_permission) | Grant a permission on an object to a role |
[`REVOKE PERMISSION`](ddl_revoke_permission) | Revoke a permission on an object from a role |
[`GRANT ROLE`](ddl_grant_role) | Grant a role to another role |
[`REVOKE ROLE`](ddl_revoke_role) | Revoke a role from another role |

- The following table lists all security statements that are not yet implemented.

Primitive Type |
---------------|
`LIST ROLES` |
`LIST PERMISSIONS` |

## DML statements

Data manipulation language (DML) statements are used to read from and write to the existing database objects. YugabyteDB implicitly commits any updates by DML statements (similar to how Apache Cassandra behaves).

Statement | Description |
----------|-------------|
[`INSERT`](dml_insert) | Insert rows into a table |
[`SELECT`](dml_select) | Select rows from a table |
[`UPDATE`](dml_update) | Update rows in a table |
[`DELETE`](dml_delete) | Delete specific rows from a table |
[`TRANSACTION`](dml_transaction) | Makes changes to multiple rows in one or more tables in a transaction |
[`TRUNCATE`](dml_truncate) | Remove all rows from a table |

## Expressions

An expression is a finite combination of one or more values, operators, functions, and expressions that specifies a computation. Expressions can be used in the following components.

- The select list of [`SELECT`](dml_select) statement. For example, `SELECT id + 1 FROM sample_table;`.
- The WHERE clause in [`SELECT`](dml_select), [`DELETE`](dml_delete), [`INSERT`](dml_insert), or [`UPDATE`](dml_update).
- The IF clause in [`DELETE`](dml_delete), [`INSERT`](dml_insert), or [`UPDATE`](dml_update).
- The VALUES clause in [`INSERT`](dml_insert).
- The SET clause in [`UPDATE`](dml_update).

Currently, the following expressions are supported.

Expression | Description |
-----------|-------------|
[Simple Value](expr_simple) | Column, constant, or null. Column alias cannot be used in expression yet. |
[Subscript `[]`](expr_subscript) | Subscripting columns of collection data types |
[Operator Call](expr_ocall) | Builtin operators only |
[Function Call](expr_fcall) | Builtin function calls only |

## Data types

- The following table lists all supported primitive types.

Primitive Type | Allowed in Key | Type Parameters | Description |
---------------|----------------|-----------------|-------------|
[`BIGINT`](type_int) | Yes | - | 64-bit signed integer |
[`BLOB`](type_blob) | Yes | - | String of binary characters |
[`BOOLEAN`](type_bool) | Yes | - | Boolean |
[`COUNTER`](type_int) | No | - | 64-bit signed integer |
[`DECIMAL`](type_number) | Yes | - | Exact, arbitrary-precision number, no upper-bound on decimal precision |
[`DATE`](type_datetime) | Yes | - | Date |
[`DOUBLE`](type_number) | Yes | - | 64-bit, inexact, floating-point number |
[`FLOAT`](type_number) | Yes | - | 64-bit, inexact, floating-point number |
[`FROZEN`](type_frozen) | Yes | 1 | Collection in binary format |
[`INET`](type_inet) | Yes | - | String representation of IP address |
[`INT` &#124; `INTEGER`](type_int) | Yes | - | 32-bit signed integer |
[`LIST`](type_collection) | No | 1 | Collection of ordered elements |
[`MAP`](type_collection) | No | 2 | Collection of pairs of key-and-value elements |
[`SET`](type_collection) | No | 1 | Collection of unique elements |
[`SMALLINT`](type_int) | Yes | - | 16-bit signed integer |
[`TEXT` &#124; `VARCHAR`](type_text) | Yes | - | String of Unicode characters |
[`TIME`](type_datetime) | Yes | - | Time of day |
[`TIMESTAMP`](type_datetime) | Yes | - | Date-and-time |
[`TIMEUUID`](type_uuid) | Yes | - | Timed UUID |
[`TINYINT`](type_int) | Yes | - | 8-bit signed integer |
[`UUID`](type_uuid) | Yes | - | Standard UUID |
[`VARINT`](type_int) | Yes | - | Arbitrary-precision integer |
[`JSONB`](type_jsonb) | No | - | JSON data type similar to PostgreSQL jsonb |

- [User-defined data types](ddl_create_type) are also supported.

- The following table lists all YCQL primitive types that are not yet implemented.

Primitive Type |
---------------|
`TUPLE` |
