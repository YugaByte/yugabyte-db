---
title: The YugabyteDB SQL language [YSQL]
headerTitle: The YugabyteDB SQL language
linkTitle: The SQL language
description: The YugabyteDB SQL language—DDL; DML; DCL; TCL; session and system control; performance control
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: the-sql-language
    parent: api-ysql
    weight: 10
aliases:
  - /latest/api/ysql/with-clause/
isTocNested: true
showAsideToc: true
---

This page describes the categorization scheme for the SQL statements and links to lists of the statements that fall into each category. It also describes notions, like the `WITH` clause, that need careful explanation and have applicability across two or more statement kinds.

{{< note title="Under construction." >}}

Future versions of the YSQL documentation will explain further such common notions.

{{< /note >}}

## Classification of SQL statements

### Data definition language ([DDL](./statements/#data-definition-language-ddl))

DDL statements define the structures in a database, change their definitions, and remove them by using `CREATE`, `ALTER`, and `DROP` commands respectively.

### Data manipulation language ([DML](./statements/#data-manipulation-language-dml))

DML statements query and modify the contents of a database.

### Data control language ([DCL](./statements/#data-control-language-dcl))

DCL statements protect the definitions of database objects and the data the tables store using a regime of rules and privileges that control the scope and power of DDL and DML statements.

### Transaction control language ([TCL](./statements/#transaction-control-language-tcl))

TCL statements manage transactions of operations on the database.

### Session and system control: [here](./statements/#session-and-system-control)

Statements in this class allow database parameters to be set at the session or the system level.

### Performance control: [here](./statements/#performance-control)

Statements in this class support the preparation of SQL statements, and their subsequent execution, to allow a more efficient execution by _binding_ actual arguments to placeholders in a SQL statement that is compiled just once, per session, rather than _every_ time actual arguments are presented. The canonical example of this feature is provided by the actual arguments that a `WHERE` clause restriction uses or the actual values than an `INSERT` statement will use.

In the performance control class, the [`EXPLAIN`](./statements/perf_explain/) statement shows what access methods a DML statement will use and (for statements with joins) the join order and method.

## The WITH clause: [here](./with-clause)

The `WITH` clause (sometimes known as the _common table expression_) can be used as part of a `SELECT` statement, an `INSERT` statement, an `UPDATE` statement, or a `DELETE` statement. For this reason, the functionality is described in a dedicated section.