---
title: Statements
headerTitle: Statements
description: List of PostgreSQL-compatible SQL statements supported by Yugabyte SQL (YSQL).
headcontent:
image: /images/section_icons/api/ysql.png
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-commands
    parent: api-ysql
    weight: 4100
isTocNested: true
showAsideToc: true
---

Yugabyte Structured Query Language (YSQL), the PostgreSQL-compatible SQL dialect for YugabyteDB, supports the following SQL statements.

| Statement | Description |
|-----------|-------------|
| [`ABORT`](txn_abort) | Rolls back a transaction |
| [`ALTER DATABASE`](ddl_alter_db) | Changes database definition |
| [`ALTER DEFAULT PRIVILEGES`](dcl_alter_default_privileges) | Defines default access privileges |
| [`ALTER DOMAIN`](ddl_alter_domain) | Alters a domain |
| [`ALTER GROUP`](dcl_alter_group) | Alter a group |
| [`ALTER POLICY`](dcl_alter_policy) | Alter a row level security policy |
| [`ALTER ROLE`](dcl_alter_role) | Alter a role |
| [`ALTER SEQUENCE`](ddl_alter_sequence) | Alters a sequence definition |
| [`ALTER TABLE`](ddl_alter_table) | Changes table definition |
| [`ALTER USER`](dcl_alter_user) | Alter a user (role) |
| [`BEGIN`](txn_begin) | Starts a transaction |
| [`COMMENT`](ddl_comment) | Adds a comment on a database object |
| [`COMMIT`](txn_commit) | Commits a transaction |
| [`COPY`](cmd_copy) | Copy data between tables and files |
| [`CREATE AGGREGATE`](ddl_create_aggregate) | Create a new aggregate |
| [`CREATE CAST`](ddl_create_cast) | Create a new cast |
| [`CREATE DATABASE`](ddl_create_database) | Create a new database |
| [`CREATE DOMAIN`](ddl_create_domain) | Create a new domain |
| [`CREATE EXTENSION`](ddl_create_extension) | Load an extension |
| [`CREATE FUNCTION`](ddl_create_function) | Create a new function |
| [`CREATE INDEX`](ddl_create_index) | Create a new index |
| [`CREATE GROUP`](dcl_create_group) | Create a new group (role) |
| [`CREATE OPERATOR`](ddl_create_operator) | Create a new operator |
| [`CREATE OPERATOR CLASS`](ddl_create_operator_class) | Create a new operator class |
| [`CREATE POLICY`](dcl_create_policy) | Create a new row level security policy |
| [`CREATE PROCEDURE`](ddl_create_procedure) | Create a new procedure |
| [`CREATE ROLE`](dcl_create_role) | Create a new role (user or group) |
| [`CREATE RULE`](ddl_create_rule) | Create a new rule |
| [`CREATE USER`](dcl_create_user) | Create a new user (role) |
| [`CREATE SCHEMA`](ddl_create_schema) | Create a new schema (namespace) |
| [`CREATE SEQUENCE`](ddl_create_sequence) | Create a new sequence generator |
| [`CREATE TABLE`](ddl_create_table) | Create a new table |
| [`CREATE TABLE AS`](ddl_create_table_as) | Create a new table |
| [`CREATE TRIGGER`](ddl_create_trigger) | Create a new trigger |
| [`CREATE TYPE`](ddl_create_type) | Create a new type |
| [`CREATE USER`](dcl_create_user) | Create a new user (role) |
| [`CREATE VIEW`](ddl_create_view) | Create a new view |
| [`DEALLOCATE`](perf_deallocate) | Deallocate a prepared statement |
| [`DELETE`](dml_delete) | Delete rows from a table |
| [`DO`](cmd_do) | Execute an anonymous code block |
| [`DROP AGGREGATE`](ddl_drop_aggregate) | Delete an aggregate |
| [`DROP CAST`](ddl_drop_cast) | Delete a cast |
| [`DROP DATABASE`](ddl_drop_database) | Delete a database from the system |
| [`DROP DOMAIN`](ddl_drop_domain) | Delete a domain |
| [`DROP EXTENSION`](ddl_drop_extension) | Delete an extension |
| [`DROP FUNCTION`](ddl_drop_function) | Delete a function |
| [`DROP GROUP`](dcl_drop_group) | Delete a group (role) |
| [`DROP OPERATOR`](ddl_drop_operator) | Delete an operator |
| [`DROP OPERATOR CLASS`](ddl_drop_operator_class) | Delete an operator class |
| [`DROP OWNED`](dcl_drop_owned) | Delete objects owned by role |
| [`DROP POLICY`](dcl_drop_policy) | Delete a row level security policy |
| [`DROP PROCEDURE`](ddl_drop_procedure) | Delete a procedure |
| [`DROP ROLE`](dcl_drop_role) | Delete a role (user or group) |
| [`DROP RULE`](ddl_drop_rule) | Delete a rule |
| [`DROP SEQUENCE`](ddl_drop_sequence) | Delete a sequence generator |
| [`DROP TABLE`](ddl_drop_table) | Deletes a table from a database |
| [`DROP TRIGGER`](ddl_drop_trigger) | Delete a trigger |
| [`DROP TYPE`](ddl_drop_type) | Delete a user-defined type |
| [`DROP USER`](dcl_drop_user) | Delete a user (role) |
| [`END`](txn_end) | Commit a transaction |
| [`EXECUTE`](perf_execute) | Execute a prepared statement |
| [`EXPLAIN`](perf_explain) | Display execution plan for a statement |
| [`INSERT`](dml_insert) | Insert rows into a table |
| [`LOCK`](txn_lock) | Locks a table |
| [`PREPARE`](perf_prepare) | Prepare a statement |
| [`REASSIGN OWNED`](dcl_reassign_owned) | Reassign owned objects |
| [`RESET`](cmd_reset) | Reset a parameter to factory settings |
| [`REVOKE`](dcl_revoke) | Remove access privileges |
| [`ROLLBACK`](txn_rollback) | Rollback a transaction |
| [`SELECT`](dml_select) | Select rows from a table |
| [`SET`](cmd_set) | Set a system, session, or transactional parameter |
| [`SET CONSTRAINTS`](txn_set_constraints) | Set constraints on current transaction |
| [`SET ROLE`](dcl_set_role) | Set a role |
| [`SET SESSION AUTHORIZATION`](dcl_set_session_authorization) | Set session authorization |
| [`SET TRANSACTION`](txn_set) | Set transaction behaviors |
| [`SHOW`](cmd_show) | Show value of a system, session, or transactional parameter |
| [`SHOW TRANSACTION`](txn_show) | Show properties of a transaction |
| [`TRUNCATE`](ddl_truncate) | Clear all rows from a table |
| [`UPDATE`](dml_update) | Update rows in a table |
