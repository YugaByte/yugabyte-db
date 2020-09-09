---
title: YSQL - Yugabyte SQL for distributed databases (PostgreSQL-compatible)
headerTitle: Yugabyte Structured Query Language (YSQL)
linkTitle: YSQL
description: Yugabyte Structured Query Language (YSQL) is the distributed SQL API for PostgreSQL-compatible YugabyteDB.
summary: Reference for the YSQL API
image: /images/section_icons/api/ysql.png
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql
    parent: api
    weight: 2900
isTocNested: true
showAsideToc: true
---

## Introduction

Yugabyte Structured Query Language (YSQL) is an ANSI SQL, fully-relational API that is best fit for scale-out RDBMS applications needing ultra resilience, massive write scalability and geographic data distribution. Currently, YSQL is compatible with PostgreSQL 11.2 version and is in fact built by reusing PostgreSQL's native query layer. It supports all traditional relational modeling features, such as referential integrity (such as foreign keys), JOINs, distributed transactions, partial indexes, triggers and stored procedures.

The main components of YSQL include the data definition language (DDL), the data manipulation language (DML), and the data control language (DCL). A number of elements are used to construct these components, including data types, database objects, names and qualifiers, expressions, and comments. Other components are also provided for different purposes such as system control, transaction control, and performance tuning.

## Quick Start

You can explore the basics of the YSQL API using the [Quick Start](../../quick-start/explore-ysql) steps.

## Data definition language (DDL)

DDL statements define the structures in a database, change their definitions, as well as remove them by using CREATE, ALTER, and DROP commands respectively.

| Statement | Description |
|-----------|-------------|
| [`ALTER DATABASE`](commands/ddl_alter_db) | Change database definition |
| [`ALTER SEQUENCE`](commands/ddl_alter_sequence) | Change sequence definition |
| [`ALTER TABLE`](commands/ddl_alter_table) | Change table definition |
| [`CREATE AGGREGATE`](commands/ddl_create_aggregate) | Create a new aggregate |
| [`CREATE CAST`](commands/ddl_create_cast) | Create a new cast |
| [`CREATE DATABASE`](commands/ddl_create_database) | Create a new database |
| [`CREATE EXTENSION`](commands/ddl_create_extension) | Load an extension |
| [`CREATE FUNCTION`](commands/ddl_create_function) | Create a new function |
| [`CREATE INDEX`](commands/ddl_create_index) | Create a new index |
| [`CREATE OPERATOR`](commands/ddl_create_operator) | Create a new operator |
| [`CREATE OPERATOR CLASS`](commands/ddl_create_operator_class) | Create a new operator class |
| [`CREATE PROCEDURE`](commands/ddl_create_procedure) | Create a new procedure |
| [`CREATE RULE`](commands/ddl_create_rule) | Create a new rule |
| [`CREATE SCHEMA`](commands/ddl_create_schema) | Create a new schema (namespace) |
| [`CREATE SEQUENCE`](commands/ddl_create_sequence) | Create a new sequence generator |
| [`CREATE TABLE`](commands/ddl_create_table) | Create a new table |
| [`CREATE TABLE AS`](commands/ddl_create_table_as) | Create a new table |
| [`CREATE TRIGGER`](commands/ddl_create_trigger) | Create a new trigger |
| [`CREATE TYPE`](commands/ddl_create_type) | Create a new type |
| [`CREATE VIEW`](commands/ddl_create_view) | Create a new view |
| [`DROP AGGREGATE`](commands/ddl_drop_aggregate) | Delete an aggregate |
| [`DROP CAST`](commands/ddl_drop_cast) | Delete a cast |
| [`DROP DATABASE`](commands/ddl_drop_database) | Delete a database from the system |
| [`DROP EXTENSION`](commands/ddl_drop_extension) | Delete an extension |
| [`DROP FUNCTION`](commands/ddl_drop_function) | Delete a function |
| [`DROP OPERATOR`](commands/ddl_drop_operator) | Delete an operator |
| [`DROP OPERATOR CLASS`](commands/ddl_drop_operator_class) | Delete an operator class |
| [`DROP PROCEDURE`](commands/ddl_drop_procedure) | Delete a procedure |
| [`DROP RULE`](commands/ddl_drop_rule) | Delete a rule |
| [`DROP SEQUENCE`](commands/ddl_drop_sequence) | Delete a sequence generator |
| [`DROP TABLE`](commands/ddl_drop_table) | Delete a table from a database |
| [`DROP TYPE`](commands/ddl_drop_type) | Delete a user-defined type |
| [`DROP TRIGGER`](commands/ddl_drop_trigger) | Delete a trigger |
| [`TRUNCATE`](commands/ddl_truncate) | Clear all rows from a table |

## Data manipulation language (DML)

DML statements modify the contents of a database.

| Statement | Description |
|-----------|-------------|
| [`DELETE`](commands/dml_delete) | Delete rows from a table |
| [`INSERT`](commands/dml_insert) | Insert rows into a table |
| [`SELECT`](commands/dml_select) | Select rows from a table |
| [`UPDATE`](commands/dml_update) | Update rows in a table |

## Data control language (DCL)

DCL statements protect and prevent the database from corruptions.

| Statement | Description |
|-----------|-------------|
| [`ALTER DEFAULT PRIVILEGES`](commands/dcl_alter_default_privileges) | Define default privileges |
| [`ALTER GROUP`](commands/dcl_alter_group) | Alter a group |
| [`ALTER POLICY`](commands/dcl_alter_policy) | Alter a row level security policy |
| [`ALTER ROLE`](commands/dcl_alter_role) | Alter a role (user or group) |
| [`ALTER USER`](commands/dcl_alter_user) | Alter a user |
| [`CREATE GROUP`](commands/dcl_create_group) | Create a new group (role) |
| [`CREATE POLICY`](commands/dcl_create_policy) | Create a new row level security policy |
| [`CREATE ROLE`](commands/dcl_create_role) | Create a new role (user or group) |
| [`CREATE USER`](commands/dcl_create_user) | Create a new user (role) |
| [`DROP GROUP`](commands/dcl_drop_group) | Drop a group |
| [`DROP POLICY`](commands/dcl_drop_policy) | Drop a row level security policy |
| [`DROP ROLE`](commands/dcl_drop_role) | Drop a role (user or group) |
| [`DROP OWNED`](commands/dcl_drop_owned) | Drop owned objects |
| [`DROP USER`](commands/dcl_drop_user) | Drop a user |
| [`GRANT`](commands/dcl_grant) | Grant permissions |
| [`REASSIGN OWNED`](commands/dcl_reassign_owned) | Reassign owned objects |
| [`REVOKE`](commands/dcl_revoke) | Revoke permissions |
| [`SET ROLE`](commands/dcl_set_role) | Set a role |
| [`SET SESSION AUTHORIZATION`](commands/dcl_set_session_authorization) | Set session authorization |

## Transaction control language (TCL)

TCL statements manage transactions of operations on the database.

| Statement | Description |
|-----------|-------------|
| [`ABORT`](commands/txn_abort) | Roll back a transaction |
| [`BEGIN`](commands/txn_begin) | Start a transaction |
| [`COMMIT`](commands/txn_commit) | Commit a transaction |
| [`END`](commands/txn_end) | Commit a transaction |
| [`ROLLBACK`](commands/txn_rollback) | Roll back a transaction |
| [`SET CONSTRAINTS`](commands/txn_set_constraints) | Set constraints on current transaction|
| [`SET TRANSACTION`](commands/txn_set) | Set transaction behaviors |
| [`SHOW TRANSACTION`](commands/txn_show) | Show properties of a transaction |

## Session and system control

| Statement | Description |
|-----------|-------------|
| [`RESET`](commands/cmd_reset) | Reset a parameter to factory settings |
| [`SET`](commands/cmd_set) | Set a system, session, or transactional parameter |
| [`SHOW`](commands/cmd_show) | Show value of a system, session, or transactional parameter |

## Performance control

| Statement | Description |
|-----------|-------------|
| [`DEALLOCATE`](commands/perf_deallocate) | Deallocate a prepared statement |
| [`EXECUTE`](commands/perf_execute) | Execute a prepared statement |
| [`EXPLAIN`](commands/perf_explain) | Explain an execution plan for a statement |
| [`PREPARE`](commands/perf_prepare) | Prepare a statement |

## Other statements

| Statement | Description |
|-----------|-------------|
| [`COPY`](commands/cmd_copy) | Copy data between tables and files |
| [`DO`](commands/cmd_do) | Execute an anonymous code block |

## Language elements

This section lists the main elements of YSQL.

- [Keywords](keywords).
- Names and Qualifiers: Some names are reserved for the system. List of [reserved names](reserved_names).
- Data types: Most PostgreSQL-compatible data types are supported. List of [data types](datatypes).
- [Expressions](exprs)
- Database Objects
- Comments
