# What Features Does YSQL Support?

YSQL uses the query layer from **PostgreSQL v11.2**, and intends to support most PostgreSQL functionality on top of a distributed database. This document tracks what features are currently supported, and which ones are in the Roadmap.

> **Note**: This list is updated periodically but not always up-to-date. Please ask in our [community Slack chat](https://www.yugabyte.com/slack) if you have any questions about specifics or the latest state. We release software almost once a week, and features are rapidly getting added.

## PostgreSQL Feature Support

Here are the features currently supported as of YugabyteDB v2.3, Oct 2020. This list also indicates what is planned for YugabyteDB v2.5 coming out around the end of November.

- [ ] All data types
    - [x] Basic types
    - [x] Autoincrement IDs (various `SERIAL` types)
    - [x] Arrays, NULLs in Arrays
    - [x] `JSONB`, `JSONB`-modifying operators and functions
    - [x] User-defined types
        - [x] Base types
        - [x] Enumerated types
        - [x] Range types
        - [x] Composite (record) types
        - [x] Array types
    - [ ] XML data type
- [x] DDL Statements
    - [x] Tables with UNIQUE KEY, PRIMARY KEY, CONSTRAINTS
    - [x] Temporary tables
    - [x] ALTER table to add columns
- [x] Queries
    - [x] FROM, WHERE, GROUP BY, HAVING, DISTINCT, LIMIT/OFFSET, WITH queries
    - [x] `EXPLAIN` query plans
    - [x] JOINs (INNER/OUTER, LEFT/RIGHT)
    - [x] Expressions and Operators
    - [x] Common Table Expressions (CTE) and Recursive Queries
    - [x] Upserts (`INSERT ... ON CONFLICT DO NOTHING/UPDATE`)
    - [x] Window functions
- [x] All Constraints
    - [x] Primary Key
    - [x] Foreign Key
    - [x] CHECK, NOT NULL, other basic constraints
    - [x] Deferrable Foreign Key constraints
    - [ ] Deferrable Primary Key and Unique constraints
    - [ ] Exclusion constraints
- [ ] Indexes
    - [x] Secondary indexes
    - [x] Partial indexes
    - [ ] GIN Indexes
    - [ ] GIST Indexes
- [ ] Transactions
    - [x] TRANSACTION blocks
    - [x] Serializable and Snapshot Isolation levels
    - [ ] Savepoints
- [ ] Advanced SQL Support
    - [x] Stored procedures
    - [x] User-defined functions
    - [x] Triggers
        - [x] Row-level triggers (BEFORE, AFTER, INSTEAD OF)
        - [x] Statement-level triggers (BEFORE, AFTER, INSTEAD OF)
        - [ ] Deferrable triggers
        - [ ] Transition tables (REFERENCING clause for triggers)
    - [x] Views
    - [x] Extensions
        - [x] pgcrypto
        - [x] fuzzystrmatch
        - [x] spi module
        - [x] pg_stat_statement
    - [x] Generic extension framework
    - [x] Partitions support
    - [ ] Materialized Views
    - [ ] Foreign data wrappers

## Core DB Features

- [x] Security
    - [x] Authentication
    - [x] Role based access control
    - [x] Row-level security
    - [x] SSL Support/TLS Encryption 
    - [x] Encryption at rest
- [ ] Backup and restore
    - [x] Scan based backups using `ysql_dump`
    - [x] Distributed backups ![Generic badge](https://img.shields.io/badge/Target-v2.5-green.svg)
- [ ] Distributed, dynamic rebuilding of indexes
    - [ ] Support for simple indexes ![Generic badge](https://img.shields.io/badge/Target-v2.5-green.svg)
    - [ ] Support for unique constraints ![Generic badge](https://img.shields.io/badge/Target-v2.5-green.svg)
- Sharding and splitting
    - [x] Hash sharding support
    - [x] Range sharding support
    - [ ] Dynamic tablet splitting ![Generic badge](https://img.shields.io/badge/Target-v2.5-green.svg)
- [ ] Change data capture
    - [x] Kafka integration ![Generic badge](https://img.shields.io/badge/Status-BETA-blue.svg)
    - [ ] Generic client library for consumers
    - [ ] Elastic search integration
- [x] Colocated tables
- [ ] Geo-distributed deployments
    - [x] Three or more DCs (with sync replication)
    - [x] Master-slave (asynchronous replication)
    - [x] Multi-master (bidirectional replication, last writer wins semantics)
    - [ ] Follower reads ![Generic badge](https://img.shields.io/badge/Target-v2.5-green.svg)
    - [ ] Read replica support
    - [ ] Row level geo-partitioning ![Generic badge](https://img.shields.io/badge/Target-v2.5-green.svg)


## App Development Support

- [x] Client driver support
    - [x] Java
    - [x] Go
    - [x] NodeJS
    - [x] Python
    - [x] Ruby
    - [x] C#
    - [x] C++
    - [x] C
    - [x] Rust
- [x] ORM support
    - [x] Spring
    - [x] Hibernate
    - [x] Play
    - [x] Gorm
    - [x] Sequelize
    - [x] SQLAlchemy
    - [x] ActiveRecord
    - [x] EntityFramework
    - [x] Diesel
- [ ] Ecosystem
    - [x] GraphQL support ![Generic badge](https://img.shields.io/badge/Status-BETA-blue.svg)
    - [x] Spring Data Yugabyte ![Generic badge](https://img.shields.io/badge/Status-BETA-blue.svg)
- [ ] Tools support
    - [x] psql
    - [x] pg_dump
    - [x] pgbench
    - [x] DBeaver
    - [x] pgAdmin ![Generic badge](https://img.shields.io/badge/Status-BETA-blue.svg)
    - [x] TablePlus ![Generic badge](https://img.shields.io/badge/Status-BETA-blue.svg)

## Performance
- [ ] Parallel queries
- [ ] YCSB
    - [x] Easy to run YCSB package
    - [ ] Framework to publish YCSB performance numbers with each release
    - [ ] Available as rpm/deb/container/yum/brew
- [ ] TPCC
    - [x] Easy to run TPCC package
    - [ ] Framework to publish TPCC performance numbers with each release
    - [ ] Available as rpm/deb/container/yum/brew

[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/YSQL-Features-Supported.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
