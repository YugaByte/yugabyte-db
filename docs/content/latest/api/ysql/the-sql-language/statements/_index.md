---
title: SQL statements [YSQL]
headerTitle: Categorized list of SQL statements
linkTitle: SQL statements
description: List of PostgreSQL-compatible SQL statements supported by Yugabyte SQL (YSQL)
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: statements
    parent: the-sql-language
    weight: 100
aliases:
  - /latest/api/ysql/commands/
isTocNested: true
showAsideToc: true
---

The YSQL statements are compatible with the SQL dialect that PostgreSQL supports. The sidebar lists all of the YSQL statements in alphabetical order. The following tables list them by catagory.

## Data definition language (DDL)

| Statement                                             | Description                                               |
| ----------------------------------------------------- | --------------------------------------------------------- |
| [`ALTER DATABASE`](ddl_alter_db)                      | Change database definition                                |
| [`ALTER DOMAIN`](ddl_alter_domain)                    | Change domain definition                                  |
| [`ALTER SEQUENCE`](ddl_alter_sequence)                | Change sequence definition                                |
| [`ALTER TABLE`](ddl_alter_table)                      | Change table definition                                   |
| [`COMMENT`](ddl_comment)                              | Set, update, or remove a comment on a database object     |
| [`CREATE AGGREGATE`](ddl_create_aggregate)            | Create an aggregate                                       |
| [`CREATE CAST`](ddl_create_cast)                      | Create a cast                                             |
| [`CREATE DATABASE`](ddl_create_database)              | Create a database                                         |
| [`CREATE DOMAIN`](ddl_create_domain)                  | Create a user-defined data type with optional constraints |
| [`CREATE EXTENSION`](ddl_create_extension)            | Load an extension                                         |
| [`CREATE FUNCTION`](ddl_create_function)              | Create a function                                         |
| [`CREATE INDEX`](ddl_create_index)                    | Create an index                                           |
| [`CREATE OPERATOR`](ddl_create_operator)              | Create an operator                                        |
| [`CREATE OPERATOR CLASS`](ddl_create_operator_class)  | Create an operator class                                  |
| [`CREATE PROCEDURE`](ddl_create_procedure)            | Create a procedure                                        |
| [`CREATE RULE`](ddl_create_rule)                      | Create a rule                                             |
| [`CREATE SCHEMA`](ddl_create_schema)                  | Create a schema (namespace)                               |
| [`CREATE SEQUENCE`](ddl_create_sequence)              | Create a sequence generator                               |
| [`CREATE TABLE`](ddl_create_table)                    | Create an empty table                                     |
| [`CREATE TABLE AS`](ddl_create_table_as)              | Create a table from the results of a executing a `SELECT` |
| [`CREATE TRIGGER`](ddl_create_trigger)                | Create a trigger                                          |
| [`CREATE TYPE`](ddl_create_type)                      | Create a type                                             |
| [`CREATE VIEW`](ddl_create_view)                      | Create a view                                             |
| [`DROP AGGREGATE`](ddl_drop_aggregate)                | Delete an aggregate                                       |
| [`DROP CAST`](ddl_drop_cast)                          | Delete a cast                                             |
| [`DROP DATABASE`](ddl_drop_database)                  | Delete a database from the system                         |
| [`DROP DOMAIN`](ddl_drop_domain)                      | Delete a domain                                           |
| [`DROP EXTENSION`](ddl_drop_extension)                | Delete an extension                                       |
| [`DROP FUNCTION`](ddl_drop_function)                  | Delete a function                                         |
| [`DROP OPERATOR`](ddl_drop_operator)                  | Delete an operator                                        |
| [`DROP OPERATOR CLASS`](ddl_drop_operator_class)      | Delete an operator class                                  |
| [`DROP PROCEDURE`](ddl_drop_procedure)                | Delete a procedure                                        |
| [`DROP RULE`](ddl_drop_rule)                          | Delete a rule                                             |
| [`DROP SEQUENCE`](ddl_drop_sequence)                  | Delete a sequence generator                               |
| [`DROP TABLE`](ddl_drop_table)                        | Delete a table from a database                            |
| [`DROP TYPE`](ddl_drop_type)                          | Delete a user-defined type                                |
| [`DROP TRIGGER`](ddl_drop_trigger)                    | Delete a trigger                                          |
| [`TRUNCATE`](ddl_truncate)                            | Clear all rows from a table                               |

## Data manipulation language (DML)

| Statement              | Description              |
| ---------------------- | ------------------------ |
| [`DELETE`](dml_delete) | Delete rows from a table |
| [`INSERT`](dml_insert) | Insert rows into a table |
| [`SELECT`](dml_select) | Select rows from a table |
| [`UPDATE`](dml_update) | Update rows in a table   |

## Data control language (DCL)

| Statement                                                    | Description                            |
| ------------------------------------------------------------ | -------------------------------------- |
| [`ALTER DEFAULT PRIVILEGES`](dcl_alter_default_privileges)   | Define default privileges              |
| [`ALTER GROUP`](dcl_alter_group)                             | Alter a group                          |
| [`ALTER POLICY`](dcl_alter_policy)                           | Alter a row level security policy      |
| [`ALTER ROLE`](dcl_alter_role)                               | Alter a role (user or group)           |
| [`ALTER USER`](dcl_alter_user)                               | Alter a user                           |
| [`CREATE GROUP`](dcl_create_group)                           | Create a group (role)                  |
| [`CREATE POLICY`](dcl_create_policy)                         | Create a row level security policy     |
| [`CREATE ROLE`](dcl_create_role)                             | Create a role (user or group)          |
| [`CREATE USER`](dcl_create_user)                             | Create a user (role)                   |
| [`DROP GROUP`](dcl_drop_group)                               | Drop a group                           |
| [`DROP POLICY`](dcl_drop_policy)                             | Drop a row level security policy       |
| [`DROP ROLE`](dcl_drop_role)                                 | Drop a role (user or group)            |
| [`DROP OWNED`](dcl_drop_owned)                               | Drop owned objects                     |
| [`DROP USER`](dcl_drop_user)                                 | Drop a user                            |
| [`GRANT`](dcl_grant)                                         | Grant permissions                      |
| [`REASSIGN OWNED`](dcl_reassign_owned)                       | Reassign owned objects                 |
| [`REVOKE`](dcl_revoke)                                       | Revoke permissions                     |
| [`SET ROLE`](dcl_set_role)                                   | Set a role                             |
| [`SET SESSION AUTHORIZATION`](dcl_set_session_authorization) | Set session authorization              |

## Transaction control language (TCL)

| Statement                                | Description                            |
| ---------------------------------------- | -------------------------------------- |
| [`ABORT`](txn_abort)                     | Roll back a transaction                |
| [`BEGIN`](txn_begin)                     | Start a transaction                    |
| [`COMMIT`](txn_commit)                   | Commit a transaction                   |
| [`END`](txn_end)                         | Commit a transaction                   |
| [`LOCK`](txn_lock)                       | Lock a table                           |
| [`ROLLBACK`](txn_rollback)               | Roll back a transaction                |
| [`SET CONSTRAINTS`](txn_set_constraints) | Set constraints on current transaction |
| [`SET TRANSACTION`](txn_set)             | Set transaction behaviors              |
| [`SHOW TRANSACTION`](txn_show)           | Show properties of a transaction       |

## Session and system control

| Statement            | Description                                                 |
| -------------------- | ----------------------------------------------------------- |
| [`RESET`](cmd_reset) | Reset a parameter to factory settings                       |
| [`SET`](cmd_set)     | Set a system, session, or transactional parameter           |
| [`SHOW`](cmd_show)   | Show value of a system, session, or transactional parameter |

## Performance control

| Statement                       | Description                               |
| ------------------------------- | ----------------------------------------- |
| [`DEALLOCATE`](perf_deallocate) | Deallocate a prepared statement           |
| [`EXECUTE`](perf_execute)       | Execute a prepared statement              |
| [`EXPLAIN`](perf_explain)       | Explain an execution plan for a statement |
| [`PREPARE`](perf_prepare)       | Prepare a statement                       |

## Other statements

| Statement          | Description                                 |
| ------------------ | ------------------------------------------- |
| [`COPY`](cmd_copy) | Copy data between tables and files          |
| [`DO`](cmd_do)     | Execute an anonymous PL/pgSQL code block    |
