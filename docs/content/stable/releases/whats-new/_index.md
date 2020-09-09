---
title: What's new in 2.2
headerTitle: What's new in 2.2
linkTitle: What's new in 2.2
description: Enhancements, changes, and resolved issues in the latest YugabyteDB release.
headcontent: Features, enhancements, and resolved issues in the latest release.
image: /images/section_icons/quick_start/install.png
aliases:
  - /stable/releases/
section: RELEASES
block_indexing: true
menu:
  stable:
    identifier: whats-new
    weight: 2589 
---

**Released:** July 15, 2020 (2.2.0.0-b80).

**New to YugabyteDB?** To get up and running in less than five minutes, follow [Quick start](../../quick-start/).

**Looking for earlier releases?** History of earlier releases is available in [Earlier releases](../earlier-releases/) section.  

## Downloads

### Binaries

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.2.0.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.2.0.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

### Docker

```sh
docker pull yugabytedb/yugabyte:2.2.0.0-b80
```

## Notable new features and enhancements

## YSQL

### Transactional distributed backups

YugabyteDB now supports [distributed backup and restore of YSQL databases](../../manage/backup-restore/snapshot-ysql/), including backup of all tables in a database. [#1139](https://github.com/yugabyte/yugabyte-db/issues/1139) and [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)

### Online index backfills

- YugabyteDB can now build indexes on non-empty tables while online, without failing other concurrent writes. When you add a new index to a table that is already populated with data, you can now use the YSQL [`CREATE INDEX`](../../api/ysql/commands/ddl_create_index/#semantics) statement to enable building these indexes in an online manner, without requiring downtime. For details how online backfill of indexes works, see the [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md) design document.
- Backfilling an index while online is disable by default. To enable online index backfilling, set the `yb-tserver` [`--ysql_disable_index_backfill`](../../reference/configuration/yb-tserver/#ysql-disable-index-backfill) flag to `false` when starting YB-TServers. Note: Do not use this flag in a production cluster yet. For details on how this works, see [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md)

### Colocated tables

Database-level colocation for YSQL, which started as a beta feature in the 2.1 release, is **now generally available** in the 2.2 release. Traditional RDBMS modeling of parent-child relationships as foreign key constraints can lead to high-latency `JOIN` queries in a geo-distributed SQL database. This is because the tablets (or shards) containing the child rows might be hosted in nodes, availability zones, and regions different from the tablets containing the parent rows. By using [colocated tables](../../architecture/docdb-sharding/colocated-tables), you can let a single tablet be shared across all tables. Colocation can also be at the overall database level, where all tables of a single database are located in the same tablet and managed by the same Raft group.  Note that tables that you do not want to reside in the overall database’s tablet because of the expectation of large data volume can override the feature at table creation time and hence get independent tablets for themselves.

What happens when the “colocation” tablet containing all the tables of a database becomes too large and starts impacting performance? Check out [automatic tablet splitting [BETA]](#automatic-tablet-splitting-beta).

### Deferred constraints on foreign keys

Foreign keys in YSQL now support the [DEFERRABLE INITIALLY IMMEDIATE](../../api/ysql/commands/ddl_create_table/#foreign-key) and [DEFERRABLE INITIALLY DEFERRED](../../api/ysql/commands/ddl_create_table/#foreign-key) clauses. Work on deferring additional constraints, including those for primary keys, is in progress. 

Application developers often declare constraints that their data must obey, leaving it to relational databases to enforce the rules. The end result is simpler application logic, lower error probability, and higher developer productivity. Automatic constraint enforcement is a powerful feature that should be leveraged whenever possible. There are times, however, when you need to temporarily defer enforcement. An example is during the data load of a relational schema where there are cyclic foreign key dependencies. Data migration tools usually defer the enforcement of foreign key dependencies to the end of a transaction by which data for all foreign keys would ideally be present.  This should also allow YSQL to power Django apps.

### yugabyted for single-node clusters

The `yugabyted` server is now out of beta for single-node deployments. New users can start using YugabyteDB without needing to understand the underlying architectures acts as a parent server, reducing the need to understand data management by YB-TServers and metadata management by YB-Masters.

See it in action by following the updated [Quick start](../../quick-start). For details, see [`yugabyted`](../../reference/configuration/yugabyted) in the Reference section.

### Automatic tablet splitting [BETA]

- YugabyteDB now supports automatic tablet splitting, resharding your data by changing the number of tablets at runtime. For details, see [Tablet splitting](../../architecture/docdb-sharding/tablet-splitting) and [Automatic Resharding of Data with Tablet Splitting](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md). [#1004](https://github.com/yugabyte/yugabyte-db/issues/1004), [#1461](https://github.com/yugabyte/yugabyte-db/issues/1461), and [#1462](https://github.com/yugabyte/yugabyte-db/issues/1462)
- To enable automatic tablet splitting, use the new `yb-master` [`--tablet_split_size_threshold_bytes`](../../reference/configuration/yb-master/#tablet-split-size-threshold-bytes) flag to specify the size when tablets should split.

### TPC-C benchmarking

New results are now available for benchmarking the performance of the YSQL API using the TPC-C suite. For the new TPC-C results and details on performing your own benchmark tests to evaluate YugabyteDB, see [TPC-C](../../benchmark/tpcc-ysql/).

### Other notable changes

- Support presplitting using [CREATE INDEX...SPLIT INTO](../../api/ysql/commands/ddl_create_index/#split-into) for range-partitioned table indexes. For details, see [Presplitting](../../architecture/docdb-sharding/tablet-splitting/#presplitting) [#4235](https://github.com/yugabyte/yugabyte-db/issues/4235)
- Fix crash for nested `SELECT` statements that involve null pushdown on system tables. [#4685](https://github.com/yugabyte/yugabyte-db/issues/4685)
- Fix wrong sorting order in presplit tables. [#4651](https://github.com/yugabyte/yugabyte-db/issues/4651)
- To help track down unoptimized (or "slow") queries, use the new `yb-tserver` [`--ysql_log_min_duration_statement`](../../reference/configuration/yb-tserver/#ysql-log-min-duration-statement). [#4817](https://github.com/yugabyte/yugabyte-db/issues/4817)
- Enhance the [`yb-admin list_tables`](../../admin/yb-admin/#list-tables) command with optional flags for listing tables with database type (`include_db_type`), table ID (`include_table_id`), and table type (`include_table_type`). This command replaces the deprecated `yb-admin list_tables_with_db_types` command. [#4546](https://github.com/yugabyte/yugabyte-db/issues/4546)
- Add support for [`ALTER TABLE`](../../api/ysql/commands/ddl_alter_table/#) on colocated tables. [#4293](https://github.com/yugabyte/yugabyte-db/issues/4293)
- Improve logic for index delete permissions. [#4980](https://github.com/yugabyte/yugabyte-db/issues/4980)
- Add fast path option for index backfill when certain statements can create indexes without unnecessary overhead from online schema migration (for example, `CREATE TABLE` with unique column constraint). Skip index backfill for unsupported statements, including `DROP INDEX`, "CREATE UNIQUE INDEX ON`, and index create in postgres nested DDL). [#4918]
- Suppress `incomplete startup packet` messages in YSQL logs. [#4813](https://github.com/yugabyte/yugabyte-db/issues/4813)
- Improve YSQL create namespace failure handling. [#3979](https://github.com/yugabyte/yugabyte-db/issues/3979)
- Add error messages to table schema version mismatch errors so they are more understandable. [#4810]
- Increase the default DDL operations timeout to 120 seconds to allow for multi-region deployments, where a CREATE DATABASE statement can take longer than one minute. [#4762](https://github.com/yugabyte/yugabyte-db/issues/4762)

## YCQL

### Transactional distributed backups

YugabyteDB supports [distributed backup and restore of YCQL databases and tables](../../manage/backup-restore/snapshot-ysql/). [#1139](https://github.com/yugabyte/yugabyte-db/issues/1139) and [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)

### Online index backfills

- YugabyteDB can now build indexes on non-empty tables while online, without failing other concurrent writes. When you add a new index to a table that is already populated with data, you can now use the YCQL [CREATE INDEX](../../api/ycql/ddl_create_index/#synopsis) statement to enable building these indexes in an online manner, without requiring downtime. For details how online backfill of indexes works, see the [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md) design document.
- In YCQL, backfilling an index while online is enabled by default. To disable, set the `yb-tserver` [`--ycql_disable_index_backfill`](../../reference/configuration/yb-tserver/#ycql-disable-index-backfill) flag to `false` when starting YB-TServers. For details on how online index backfill works, see [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md). [#2301](https://github.com/yugabyte/yugabyte-db/issues/2301) and [#4708](https://github.com/yugabyte/yugabyte-db/issues/4708)

### Online schema changes for YCQL [BETA]

Most applications have a need to frequently evolve the database schema, while simultaneously ensuring zero downtime during those schema change operations. Therefore, there is a need for schema migrations (which involve DDL operations) to be safely run in a concurrent and online manner alongside foreground client operations. In case of a failure, the schema change should be rolled back and not leave the database in a partially modified state. With the 2.2 release, not only the overall DocDB framework for supporting such schema changes in an online and safe manner has been introduced but also this feature is now available in beta in the context of YCQL using the [`ALTER TABLE`](../../api/ycql/ddl_alter_table/) statement.

### Automatic tablet splitting [BETA]

- YugabyteDB now supports automatic tablet splitting at runtime by changing the number of tablets based on size thresholds. For details, see [Tablet splitting](../../architecture/docdb-sharding/tablet-splitting) and [Automatic Resharding of Data with Tablet Splitting](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md). [#1004](https://github.com/yugabyte/yugabyte-db/issues/1004), [#1461](https://github.com/yugabyte/yugabyte-db/issues/1461), and [#1462](https://github.com/yugabyte/yugabyte-db/issues/1462)
- To enable automatic tablet splitting, use the new `yb-master` [`--tablet_split_size_threshold_bytes`](../../reference/configuration/yb-master/#tablet-split-size-threshold-bytes) flag to specify the size when tablets should split.

### Other notable changes

- Throttle YCQL calls when soft memory limit is reached. Two new flags, `throttle_cql_calls_on_soft_memory_limit` and `throttle_cql_calls_policy` can be used to control it. [#4973](https://github.com/yugabyte/yugabyte-db/issues/4973)
- Implements a password cache to allow connections to be created more quickly from recently used accounts. Helps reduce high CPU usage when using YCQL authorization. [#4596](https://github.com/yugabyte/yugabyte-db/issues/4596)
- Fix `column doesn't exist` error when an index is created on a column which is a prefix of another column. [#4881](https://github.com/yugabyte/yugabyte-db/issues/4881)
- Fix crashes when using ORDER BY for non-existent column with index scan. [#4908](https://github.com/yugabyte/yugabyte-db/issues/4908)

## System improvements

- Add HTTP endpoints for determining master leadership and returning information on masters. [#2606](https://github.com/yugabyte/yugabyte-db/issues/2606)
  - `<web>/api/v1/masters`: Returns all master statuses.
  - `<web>/api/v1/is-leader`: Returns `200 OK` response status code when the master is a leader and `503 Service Unavailable` when the master is not a leader.
- Add HTTP endpoint `/api/v1/cluster-config` for YB-Master to return the current cluster configuration in JSON format. [#4748](https://github.com/yugabyte/yugabyte-db/issues/4748)
  - Thanks, [AbdallahKhaled93](https://github.com/AbdallahKhaled93), for your contribution!
- Output of `yb-admin get_universe_config` command is now in JSON format for easier parsing. [#4589]([#1462](https://github.com/yugabyte/yugabyte-db/issues/4589)
- Add `yugabyted destroy` command. [#3872]([#3849](https://github.com/yugabyte/yugabyte-db/issues/3872)
- Change logic used to determine if the load balancer is idle. [#4707](https://github.com/yugabyte/yugabyte-db/issues/4707)
- Default to IPv4 addresses for DNS resolution and local addresses. [#4851](https://github.com/yugabyte/yugabyte-db/issues/4293)
- Add a Grafana dashboard for YugabyteDB. [#4725](https://github.com/yugabyte/yugabyte-db/issues/4725)
- [CDC] Check for table properties equivalence when comparing schemas in 2DC setups and ignore properties that don't need to be the same. [#4233](https://github.com/yugabyte/yugabyte-db/issues/4233)
- [DocDB] Allow multiple indexes to backfill or delete simultaneously. [#2784](https://github.com/yugabyte/yugabyte-db/issues/2784)
- [DocDB] Transition to new leader gracefully during a leader stepdown. When a leader stepdown happens with no new leader candidate specified in the stepdown request, the peer simply steps down leaving the group with no leader until regular heartbeat timeouts are triggered. This change makes it so that the leader attempts to transition to the most up-to-date peer, if possible. [#4298](https://github.com/yugabyte/yugabyte-db/issues/4298)
- Update [`yb-admin list_snapshots`](../../admin/yb-admin/#list-snapshots) command to use `not_show_restored` option to exclude fully RESTORED entries. [#4351](ttps://github.com/yugabyte/yugabyte-db/issues/4351)
- [DocDB] Clean up leftover snapshot files from failed snapshots that resulted in remote bootstrap getting stuck in a failure loop. [#4745](https://github.com/yugabyte/yugabyte-db/issues/4745)
- [DocDB] Clean up metadata in memory after deleting snapshots. [#4887](https://github.com/yugabyte/yugabyte-db/issues/4877)
- [DocDB] Fix snapshots getting stuck in retry loop with deleted table. [#4610](https://github.com/yugabyte/yugabyte-db/issues/4877) and [#4302](https://github.com/yugabyte/yugabyte-db/issues/4302)
- [DocDB] When using [`yb-admin master_leader_stepdown`](../../admin/yb-admin/#master-leader-stepdown), specifying a new leader is now optional. [#4722](https://github.com/yugabyte/yugabyte-db/issues/4722)
- [DocDB] Add breakdown of disk space on TServer dashboard. [#4767](https://github.com/yugabyte/yugabyte-db/issues/4767)
- [DocDB] Suppress `yb-admin` from logging harmless "Failed to schedule invoking callback on response" warning. [#4131](https://github.com/yugabyte/yugabyte-db/issues/4131)
- [DocDB] Allow specifying IPv6 addresses in bind addresses. [#3644](https://github.com/yugabyte/yugabyte-db/issues/3644)
- [DocDB] Fix TS Heartbeater can get stuck if master network connectivity breaks. [#4838](https://github.com/yugabyte/yugabyte-db/issues/4838)
- [DocDB] Improve protection against hitting hard memory limit when follower tablet peers are lagging behind leaders on a node. (for example, node downtime because of yb-tserver restart). [#2563](https://github.com/yugabyte/yugabyte-db/issues/2563)
- `yugabyted` `--bind_ip` flag renamed to `--listen`. [#4960](https://github.com/yugabyte/yugabyte-db/issues/4960)
- Fix malformed URL for ycql rpcz when clicking **YCQL Live Ops** link in the YB-TServer utilities page. [#4886](https://github.com/yugabyte/yugabyte-db/issues/4866)
- Fix export name for YCQL metrics. [#4955](https://github.com/yugabyte/yugabyte-db/issues/4955)
- Fix YB-Master UI to show correct number of tablets for colocated tables. [#4699](https://github.com/yugabyte/yugabyte-db/issues/4699)

## Yugabyte Platform

- Add option to [back up and restore YSQL databases](../../) and universe-level transactional backups. [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)
- On the **Universes** page, the **Replication** tab displays by default and adds default state when replication lag metric query returns no data.
- Add **Replication** tab to Universe overview tab list if enabled and query for replication lag metrics. [#]
- Add replication lag metrics `async_replication_sent_lag_micros` (last applied time on producer - last polled for record) and `async_replication_committed_lag_micros` (last applied time on producer - last applied time on consumer) for 2DC replication and export to Prometheus. [#2154](https://github.com/yugabyte/yugabyte-db/issues/2154)
- Retrieve IAM Instance Profile Credentials for backups. This retrieves the `AccessKeyId` and `SecretAccessKey` and set these in the configuration so that data nodes inherit the required IAM permissions to access S3. [#3451](https://github.com/yugabyte/yugabyte-db/issues/3451) and [#4900](https://github.com/yugabyte/yugabyte-db/issues/4900)
- Add option to create a backup of multiple YCQL transactional tables. [#4540](https://github.com/yugabyte/yugabyte-db/issues/4540)
- Add support for c5d instance types. [#4914](https://github.com/yugabyte/yugabyte-db/issues/4914)
- Add support for installing epel-release in Amazon Linux. [#4561](https://github.com/yugabyte/yugabyte-db/issues/4561)
- Fixed requested TLS client certificate generated with expired date and ysqlsh fails to connect. [#4732](https://github.com/yugabyte/yugabyte-db/issues/4732)
- Fixed universe fails to create if user-supplied certificate is selected. [#4733](https://github.com/yugabyte/yugabyte-db/issues/4733)
- Implement OAuth2/SSO authentication for Yugabyte Platform sign-in. [#4633](https://github.com/yugabyte/yugabyte-db/issues/4644) and [#4420](https://github.com/yugabyte/yugabyte-db/issues/4420)
- Add backup and restore options for YSQL tables and universe-level transactional backups. [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)
- Retrieve IAM Instance Profile Credentials for backups. This retrieves the `AccessKeyId` and `SecretAccessKey` and set these in the configuration so that data nodes inherit the required IAM permissions to access S3. [#4900](https://github.com/yugabyte/yugabyte-db/issues/4900)
- In the **Health and Alerting** tab of the Admin Console, the Username and Password fields in **Custom SMTP Configuration** are now optional. [#4952](https://github.com/yugabyte/yugabyte-db/issues/4952)
- Fix password reset in customer profile page. [#4666](https://github.com/yugabyte/yugabyte-db/issues/4666) and [#3909](https://github.com/yugabyte/yugabyte-db/issues/3909)
- Fix **Backups** page not loading when there is a pending task. [#4754](https://github.com/yugabyte/yugabyte-db/issues/4754)
- Change UI text displaying "GFlag" to "Flag" in the Admin Console. [#4659](https://github.com/yugabyte/yugabyte-db/issues/4659)

{{< note title="Note" >}}

Prior to version 2.0, YSQL was still in beta. As a result, the 2.0 release included a backward-incompatible file format change for YSQL. If you have an existing cluster running releases earlier than 2.0 with YSQL enabled, then you will not be able to upgrade to version 2.0+. Export from your existing cluster and then import into a new cluster (v2.0 or later) to use existing data.

{{< /note >}}
