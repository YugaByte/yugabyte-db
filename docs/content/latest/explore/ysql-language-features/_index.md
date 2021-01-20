---
title: YSQL vs PostgreSQL
headerTitle: YSQL vs PostgreSQL
linkTitle: YSQL vs PostgreSQL
description: PostgreSQL features in YSQL.
headcontent: PostgreSQL features in YSQL.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: explore-ysql-language-features
    parent: explore
    weight: 200
isTocNested: true
showAsideToc: true
---


The YSQ API of YugabyteDB reuses a fork of the query layer of PostgreSQL as its starting point, and runs on top of YugabyteDB’s distributed storage layer called DocDB. This architectural decision means that YSQL supports most PostgreSQL features (data types, queries, expressions, operators and functions, stored procedures, triggers, extensions, etc).

{{< tip title="Tip" >}}
A large portion of the documentation and examples written for PostgreSQL would work against YugabyteDB.

**Why is this true?** The reuse of the *upper half* of PostgreSQL means that applications in effect interact with the PostgreSQL query layer. Thus, not only are most of the features supported, they also working exactly the same way as they would in PostgreSQL.  
{{< /tip >}}


The figure below diagrammatically shows how the query layer of PostgreSQL is reused - specifically components that receive the query (*postman*), the query *parser* / *rewriter* / *analyzer*, as well as *planning* and *executing* the query. Some of these components have been modified to work efficiently as a distributed SQL database.

![Reusing the PostgreSQL query layer in YSQL](/images/section_icons/architecture/Reusing-PostgreSQL-query-layer.png)

## PostgreSQL features in YSQL

This section walks through some of the features in YSQL. If you have worked with PostgreSQL, you should find most of these features and how they function very familiar.

|       Feature in YSQL        |              Description of feature                       |
|------------------------------|-----------------------------------------------------------|
| <span style="font-size:16px">[Basics](databases-schemas-tables/)</span>  | SQL shell with `ysqlsh`, users, databases, tables and schemas |
| <span style="font-size:16px">[Data types](data-types/)</span>            | String / numeric / temporal types, `SERIAL` pseudo type, `ENUM`, arrays, and composite types |
<!--
| <span style="font-size:16px">[Data Manipulation](data-manipulation/)</span> | `INSERT`, `UPDATE`, `DELETE`, `INSERT ... ON CONFLICT` and `RETURNING` clauses, etc. |
| <span style="font-size:16px">[Queries and Joins](queries/)</span>           | Joins, `FROM` / `GROUP BY` / `HAVING` clauses, common table expressions, recursive queries, etc. |
| <span style="font-size:16px">[Functions and operators](functions-operators/)</span> | Conditional expressions, math / string / date / time / window functions and operators  |
| <span style="font-size:16px">[Stored Procedures](stored-procedures/)</span> | Support for the various stored procedures |
| <span style="font-size:16px">[Triggers](triggers/)</span>                   | Triggers (on data modification) and event triggers (on schema changes) |
| <span style="font-size:16px">[Table Partitions](table-partitions)</span>    | List, range and hash partitioning of tables               |
| <span style="font-size:16px">[Advanced Topics](advanced-topics/)</span>     | Using `VIEWS`, PostgreSQL extensions supported in YSQL, temporary tables, etc. |
-->
The following topics are covered in separate sections:

* [Document data types (`JSONB` and `JSON`)](../json-support)
* [Distributed transactions](../transactions)
<!--
* [Indexes and constraints]()
-->

## What's extra in YSQL?

Since YugabyteDB is a distributed SQL database, there are some features that make more sense for YSQL and hence are not present in PostgreSQL. These are outlined below.

|     Feature in YSQL          |        Description of feature                             |
|------------------------------|-----------------------------------------------------------|
| <span style="font-size:16px">Data distribution with `HASH`</span> | YSQL supports a `HASH` sort order in addition to `ASC` and `DESC` for indexes |
| <span style="font-size:16px">`TABLESPACES` for geographic placement</span> | `TABLESPACES` can be used to pin data in tables and table partitions to different geographic locations. |
| <span style="font-size:16px">`TABLEGROUPS` for colocating tables</span> | `TABLEGROUPS` enables colocating multiple smaller tables into one tablet for better performance. |

<!--
Read more about these [YSQL features not present in PostgreSQL](ysql-features-not-in-postgres/).
-->