---
title: Best practices for YSQL database administrators
headerTitle: Best practices for YSQL database administrators
linkTitle: YSQL database administrators
description: Tips and tricks to build YSQL applications
headcontent: Tips and tricks for administering YSQL databases
menu:
  stable:
    identifier: best-practices-ysql-administration
    parent: best-practices-operations
    weight: 10
aliases:
  - /stable/develop/best-practices/administration/
type: docs
rightNav:
  hideH3: true
---

Database administrators can fine-tune YugabyteDB deployments for better reliability, performance, and operational efficiency by following targeted best practices. This guide outlines key recommendations for configuring single-AZ environments, optimizing memory use, accelerating CI/CD tests, and safely managing concurrent DML and DDL operations. These tips are designed to help DBAs maintain stable, scalable YSQL clusters in real-world and test scenarios alike.

## Single availability zone (AZ) deployments

In single AZ deployments, you need to set the [yb-tserver](../../reference/configuration/yb-tserver) flag `--durable_wal_write=true` to not lose data if the whole data center goes down (for example, power failure).

## Allow for tablet replica overheads

Although you can manually provision the amount of memory each TServer uses using flags ([--memory_limit_hard_bytes](../../reference/configuration/yb-tserver/#memory-limit-hard-bytes) or [--default_memory_limit_to_ram_ratio](../../reference/configuration/yb-tserver/#default-memory-limit-to-ram-ratio)), this can be tricky as you need to take into account how much memory the kernel needs, along with the PostgreSQL processes and any Master process that is going to be colocated with the TServer.

{{<note title = "Kubernetes deployments">}}
For Kubernetes universes, memory limits are controlled via resource specifications in the Helm chart. Accordingly, `--default_memory_limit_to_ram_ratio` does not apply, and `--memory_limit_hard_bytes` is automatically set from the Kubernetes pod memory limits.

See [Memory limits in Kubernetes deployments](../../deploy/kubernetes/single-zone/oss/helm-chart/#memory-limits-for-kubernetes-deployments) for details.
{{</note>}}

Accordingly, you should use the [--use_memory_defaults_optimized_for_ysql](../../reference/configuration/yb-tserver/#use-memory-defaults-optimized-for-ysql) flag, which gives good memory division settings for using YSQL, optimized for your node's size.

If this flag is true, then the [memory division flag defaults](../../reference/configuration/yb-tserver/#memory-division-flags) change to provide much more memory for PostgreSQL; furthermore, they optimize for the node size.

Note that although the default setting is false, when creating a new universe using yugabyted or YugabyteDB Anywhere, the flag is set to true, unless you explicitly set it to false.

For more details, refer to [Memory and tablet limits](../../deploy/checklist/#memory-and-tablet-limits).

## Settings for CI and CD integration tests

You can set certain flags to increase performance using YugabyteDB in CI and CD automated test scenarios as follows:

- Point the flags `--fs_data_dirs`, and `--fs_wal_dirs` to a RAMDisk directory to make DML, DDL, cluster creation, and cluster deletion faster, ensuring that data is not written to disk.
- Set the flag `--yb_num_shards_per_tserver=1`. Reducing the number of shards lowers overhead when creating or dropping YSQL tables, and writing or reading small amounts of data.
- Use colocated databases in YSQL. Colocation lowers overhead when creating or dropping YSQL tables, and writing or reading small amounts of data.
- Set the flag `--replication_factor=1` for test scenarios, as keeping the data three way replicated (default) is not necessary. Reducing that to 1 reduces space usage and increases performance.
- Use `TRUNCATE table1,table2,table3..tablen;` instead of CREATE TABLE, and DROP TABLE between test cases.

## Concurrent DML during a DDL operation

Do not run DML against a relation while DDL is modifying that relation's schema.

By default, YugabyteDB doesn't restrict DML and DDL concurrency. DML is *allowed* to execute while a DDL statement changes the schema that the DML is using. For example, `ALTER TABLE <table> ADD COLUMN` can add a column while `SELECT * FROM <table>` runs on the same relation. PostgreSQL typically prevents this by taking an ACCESS EXCLUSIVE table lock; YugabyteDB does not, unless you enable table-level locking.

Allowed does not mean correct. Concurrent DML can use a stale schema, skip newly added structures, or write rows that the DDL never sees. Some of those outcomes return errors; others succeed and silently leave wrong data.

### Retryable errors

Concurrent DML may fail with `schema mismatch` or `catalog version mismatch`. Applications should [retry those operations](https://www.yugabyte.com/blog/retry-mechanism-spring-boot-app/).

### Silent correctness issues

When concurrent DML does not error, the failures fall into these categories:

| Risk | What happens |
| :--- | :----------- |
| Stale snapshot / missed writes | DDL rewrites the table or index at a point-in-time snapshot. Concurrent DML writes to the old storage and is lost. |
| Constraint violation escapes validation | DDL scans the table to validate a constraint. Concurrent DML can insert violating rows after the scan completes but before the constraint is enforced. |
| Stale catalog / wrong routing | DDL modifies the partition descriptor or relation metadata. Concurrent DML using a cached (stale) catalog version may route rows incorrectly or miss newly added structures. |
| Stale catalog / missed trigger fire | DDL adds or enables a trigger. Concurrent DML that already cached the old trigger list does not fire the new trigger. |

### DDL operations that are unsafe with concurrent DML

| DDL operation | Issue with concurrent DML |
| :------------ | :------------------------ |
| `CREATE INDEX NONCONCURRENTLY` / `REINDEX` | The index is built by scanning the table at a snapshot. Concurrent INSERTs and UPDATEs are not inserted into the new index, so index entries are missing. See [Concurrent index creation](../../api/ysql/the-sql-language/statements/ddl_create_index/#concurrent-index-creation). |
| Partition commands (`CREATE TABLE ... PARTITION OF`, `ATTACH PARTITION`, `DETACH PARTITION`) | These modify the parent's partition descriptor. Concurrent DML using a stale cached descriptor may route rows to the wrong partition (for example, the default instead of the new partition), skip partition constraint checks, or write to a detached partition. |
| ALTER TABLE table rewrite (`ALTER COLUMN TYPE`, `ADD COLUMN ... DEFAULT <volatile expression>`, `ADD`/`DROP PRIMARY KEY`) | The table is rewritten by copying data at a snapshot. Concurrent DML writes to the old table and is not reflected in the new table — silent data loss. See [Alter table operations that involve a table rewrite](../../api/ysql/the-sql-language/statements/ddl_alter_table/#alter-table-operations-that-involve-a-table-rewrite). |
| Constraints and triggers (`ADD CONSTRAINT` CHECK/UNIQUE/FK, `SET NOT NULL`, `CREATE`/`ENABLE`/`DISABLE TRIGGER`) | Constraint validation scans the table at a snapshot. Concurrent DML can insert violating rows after the scan but before enforcement begins. For triggers, concurrent DML using a stale catalog does not fire newly added or enabled triggers. |

Most schema-only DDLs complete quickly. The operations in the table above copy or validate data, and they are not safe to overlap with DML on the same relation.

### How to avoid these issues

Pause DML on the affected relation until the DDL completes, or enable {{<tags/feature/ea idea="1114">}}[table-level locking](../../explore/transactions/explicit-locking/#table-level-locks). Table-level locks are disabled by default.

For `ALTER TABLE ADD CONSTRAINT`, you can add the constraint as `NOT VALID` and validate it in a second step. This is safe even without table locking:

```sql
ALTER TABLE ... ADD CONSTRAINT ... NOT VALID;
ALTER TABLE ... VALIDATE CONSTRAINT ...;
```

For `ALTER TABLE ... ATTACH PARTITION`, first add CHECK constraints on the existing partitions that exclude the newly attached range. That guarantees no rows are present in, or can be inserted into, that range on the existing partitions before you attach the new partition.

## Concurrent DDL during a DDL operation

Concurrent Data Definition Language (DDL) operations are currently unsupported. All DDL statements targeting the same database must be executed sequentially, one at a time, from a single database connection. DDL statements that operate on shared objects (roles, tablespaces) affect all databases in the cluster and must also be serialized. DDL statements that affect entities in different databases can be run concurrently.

Enforce DDL serialization at the application and operational level:

- Execute all DDLs sequentially from a single connection. Use a dedicated, non-pooled connection for schema migrations.
- Wait for each DDL to fully complete before issuing the next statement.
- Implement client-side retry logic for schema mismatch and catalog version mismatch errors in any [DML that may overlap with DDL windows](#concurrent-dml-during-a-ddl-operation).
- Schedule DDL during maintenance windows to minimize overlap with application DML traffic, backup jobs, and other administrative operations.
- In versions earlier than v2025.1.1, DDL verification states can block backup and restore operations; run DDL and backup jobs separately. (In v2025.2.1 and later, taking YSQL backups during DDL operations is supported by default, and backups succeed even in case of concurrent DDLs.)

## Preload PostgreSQL system catalog entries into the local catalog cache

Many common PostgreSQL operations, such as parsing a query, planning, and so on, require looking up entries in PostgreSQL system catalog tables, including pg_class, pg_operator, pg_statistic, and pg_attribute, for PostgreSQL metadata for the columns, operators, and more.

Each PostgreSQL backend (process) caches such metadata for performance reasons. In YugabyteDB, misses on these caches need to be loaded from the YB-Master leader. As a result, initial queries on that backend can be slow until these caches are warm, especially if the YB-Master leader is in a different region.

You can customize this tradeoff to control the preloading entries into PostgreSQL caches. Refer to [Customize preloading of YSQL catalog caches](../ysql-catalog-cache-tuning-guide/) for information on how to make the right tradeoffs for your application.
