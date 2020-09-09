---
title: FAQs about YugabyteDB API compatibility
headerTitle: API compatibility FAQ
linkTitle: API compatibility FAQ
description: Answers to common questions about YugabyteDB API compatibility.
aliases:
  - /faq/cassandra/
  - /latest/faq/cassandra/
menu:
  latest:
    identifier: faq-api-compatibility
    parent: faq
    weight: 2730
isTocNested: true
showAsideToc: true
---

## What does API compatibility exactly mean?

API compatibility refers to the fact that the database APIs offered by YugabyteDB servers implement the same wire protocol and modeling/query language as that of an existing database. Since client drivers, command line shells, IDE integrations and other ecosystem integrations of the existing database rely on this wire protocol and modeling/query language, they are expected to work with YugabyteDB without major modifications. 

{{< note title="Note" >}}
The [YSQL](../../api/ysql) API is compatible with PostgreSQL. This means PostgreSQL client drivers, psql command line shell, IDE integrations such as TablePlus and DBWeaver and more can be used with YugabyteDB. The same concept applies to [YCQL](../../api/ycql) in the context of the Apache Cassandra Query Language.
{{< /note >}}

## Why are YugabyteDB APIs compatible with popular DB languages?

- YugabyteDB's API compatibility is aimed at accelerating developer onboarding. By integrating well with the existing ecosystem, YugabyteDB ensures that developers can get started easily using a language they are already comfortable with. 

- YugabyteDB's API compatibility is not aimed at lift-and-shift porting of existing applications written for the original language. This is because existing applications are not written to take advantage of the distributed, strongly-consistent storage architecture that YugabyteDB provides. For such existing applications, developers should expect to modify their previously monolithic PostgreSQL and/or non-transactional Cassandra data access logic as they look to migrate to YugabyteDB.

## YSQL compatibility with PostgreSQL

### What is the extent of compatibility with PostgreSQL?

As highlighted in [Distributed PostgreSQL on a Google Spanner Architecture – Query Layer](https://blog.yugabyte.com/distributed-postgresql-on-a-google-spanner-architecture-query-layer/), YSQL reuses open source PostgreSQL’s query layer (written in C) as much as possible and as a result is wire-compatible with PostgreSQL dialect and client drivers. Specifically, YSQL is based on PostgreSQL v11.2. Following are some of the currently supported features:

- DDL statements: CREATE, DROP, and TRUNCATE tables
- Data types: All primitive types including numeric types (integers and floats), text data types, byte arrays, date-time types, UUID, SERIAL, as well as JSONB
- DML statements: INSERT, UPDATE, SELECT and DELETE. Bulk of core SQL including JOINs, WHERE clauses, GROUP BY, ORDER BY, LIMIT, OFFSET, and SEQUENCES
- Transactions: ABORT, ROLLBACK, BEGIN, END, and COMMIT
- Expressions: Rich set of PostgreSQL built-in functions and operators
- Other Features: VIEWs, EXPLAIN, PREPARE-BIND-EXECUTE, and JDBC support

YugabyteDB's goal is to remain as compatible with PostgreSQL as much as possible. If you see a feature currently missing, please file a [GitHub issue](https://github.com/yugabyte/yugabyte-db/issues) for us.

## Can I insert data using YCQL, but read using YSQL, or vice versa?

The YugabyteDB APIs are currently isolated and independent from one another. Data inserted or managed by one API cannot be queried by the other API. Additionally, Yugabyte does not provide a way to access the data across the APIs. An external framework, such as Presto, might be useful for simple use cases. For an example that joins YCQL and YSQL data, see the blog post about [Presto on YugabyteDB: Interactive OLAP SQL Queries Made Easy](https://blog.yugabyte.com/presto-on-yugabyte-db-interactive-olap-sql-queries-made-easy-facebook/).

Allowing YCQL tables to be accessed from the PostgreSQL-compatible YSQL API as foreign tables using foreign data wrappers (FDW) is on the roadmap. You can comment or increase the priority of the associated [GitHub](https://github.com/yugabyte/yugabyte-db/issues/830) issue.

## YCQL compatibility with Apache Cassandra QL

YCQL is compatible with v3.4 of Apache Cassandra QL (CQL). Following questions highlight how YCQL differs from CQL.

### Features present in YCQL but not present in CQL

1. Strongly-consistent reads and writes for a single row as an absolute guarantee. This is because YugabyteDB is a Consistent & Partition-tolerant (CP) database as opposed to Apache Cassandra which is an Available & Partition-tolerant (AP) databse. [Official Jepsen tests](https://blog.yugabyte.com/yugabyte-db-1-2-passes-jepsen-testing/) prove this correctness aspect under extreme failure conditions.
2. [JSONB](../../develop/learn/data-types/) column type for modeling document data
3. [Distributed transactions](../../develop/learn/acid-transactions/) for multi-row ACID transactions.

### Features present in both YCQL and CQL but YCQL provides stricter guarantees

1. [Secondary indexes](../../develop/learn/data-modeling/) are by default strongly consistent since internally they use distributed transactions.
2. [INTEGER](../../api/ycql/type_int/) and [COUNTER](../../api/ycql/type_int/) data types are equivalent and both can be incremented without any lightweight transactions.
3. Timeline-consistent tunably-stale reads that maintain ordering guarantees from either a follower replica in the primary cluster or a observer replica in the read replica cluster.

### CQL features that are either unnecessary or disallowed in YCQL

1. Lightweight transactions for compare-and-swap operations (such as incrementing integers) are unnecessary because YCQL achieves single row linearizability by default.
2. Tunable write consistency is disallowed in YCQL because writes are committed at quorum using Raft replication protocol.

This [blog](https://blog.yugabyte.com/apache-cassandra-lightweight-transactions-secondary-indexes-tunable-consistency/) goes into the details of YCQL vs Apache Cassandra architecture and is recommended for further reading.

### Do INSERTs do “upserts” by default? How do I insert data only if it is absent?

By default, inserts overwrite data on primary key collisions. So INSERTs do an upsert. This an intended CQL feature. In order to insert data only if the primary key is not already present,  add a clause "IF NOT EXISTS" to the INSERT statement. This will cause the INSERT fail if the row exists.

Here is an example from CQL:

```sql
INSERT INTO mycompany.users (id, lastname, firstname) 
   VALUES (100, ‘Smith’, ‘John’) 
IF NOT EXISTS;
```

### Can I have collection data types in the partition key? Will I be able to do partial matches on that collection data type?

Yes, you can have collection data types as primary keys as long as they are marked FROZEN. Collection types that are marked `FROZEN` do not support partial matches.

### What is the difference between a `COUNTER` data type and `INTEGER` data type?

Unlike Apache Cassandra, YugabyteDB COUNTER type is almost the same as INTEGER types. There is no need of lightweight transactions requiring 4 round trips to perform increments in YugabyteDB - these are efficiently performed with just one round trip.

### How is 'USING TIMESTAMP' different in YugabyteDB?

In Apache Cassandra, the highest timestamp provided always wins. Example:

INSERT with timestamp far in the future.

```sql
> INSERT INTO table (c1, c2, c3) VALUES (1, 2, 3) USING TIMESTAMP 1607681258727447;
> SELECT * FROM table;
```

```
 c1 | c2 | c3
----+----+----
  1 |  2 |  3
```

INSERT at the current timestamp does not overwrite previous value which was written at a higher
timestamp.

```sql
> INSERT INTO table (c1, c2, c3) VALUES (1, 2, 4); 
> SELECT * FROM table;
```

```
 c1 | c2 | c3
----+----+----
  1 |  2 |  3
```

On the other hand in Yugabyte, for efficiency purposes INSERTs and UPDATEs without the `USING
TIMESTAMP` clause always overwrite the older values. On the other hand, if you have the `USING
TIMESTAMP` clause, then appropriate timestamp ordering is performed. Example:

```sql
> INSERT INTO table (c1, c2, c3) VALUES (1, 2, 3) USING TIMESTAMP 1000;
> SELECT * FROM table;
```

```
 c1 | c2 | c3
----+----+----
  1 |  2 |  3
```

INSERT with timestamp far in the future, this would overwrite old value.

```sql
> INSERT INTO table (c1, c2, c3) VALUES (1, 2, 4) USING TIMESTAMP 1607681258727447;
> SELECT * FROM table;
```

```
 c1 | c2 | c3
----+----+----
  1 |  2 |  4
```

INSERT without 'USING TIMESTAMP' will always overwrite.

```sql
> INSERT INTO table (c1, c2, c3) VALUES (1, 2, 5); 
> SELECT * FROM table;
```

```
 c1 | c2 | c3
----+----+----
  1 |  2 |  5
```
