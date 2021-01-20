---
title: What's new in the current v2.2 stable release series
headerTitle: What's new in the current v2.2 stable release series
linkTitle: 2.2 (stable)
description: Enhancements, changes, and resolved issues in the current stable release series, recommended for production deployments.
headcontent: Features, enhancements, and resolved issues in the current stable release series, recommended for production deployments.
image: /images/section_icons/quick_start/install.png
aliases:
  - /stable/releases/
section: RELEASES
menu:
  stable:
    parent: whats-new
    identifier: stable-releases
    weight: 2586 
isTocNested: true
showAsideToc: true
---

Included here are the notable new features and enhancements and the release notes for all releases in the current v2.2 stable release series.

{{< note title="New release versioning" >}}

Starting with v0, Yugabyte release versions follow a [new release versioning convention](../../versioning). Stable release series are denoted by `MAJOR.EVEN`, introduce new features and changes added since the previous stable release series and are supported for production deployments. Patch releases, denoted by `MAJOR.EVEN.PATCH`, include bug fixes and revisions that do not break backwards compatibility.
For more information, see [Supported and planned releases](../../releases-overview).

{{< /note >}}

{{< note title="Upgrading from 1.3" >}}

Prior to v2.0, YSQL was still in beta. Upon release of v2.0, a backward-incompatible file format change was made for YSQL. For existing clusters running pre-2.0 release with YSQL enabled, you cannot upgrade to v2.0 or later. Instead, export your data from existing clusters and then import the data into a new cluster (v2.0 or later).

{{< /note >}}

---
## Notable features and enhancements

Here is an overview of all notable features and enhancements added to the current v2.2 stable release series, recommended for production deployments. For features added in specific releases, see [Release notes](#release-notes) below.

### YSQL

#### Transactional distributed backups

YugabyteDB now supports [distributed backup and restore of YSQL databases](../../../manage/backup-restore/snapshot-ysql/), including backup of all tables in a database. [#1139](https://github.com/yugabyte/yugabyte-db/issues/1139) and [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)

#### Online index backfills

* YugabyteDB can now build indexes on non-empty tables while online, without failing other concurrent writes. When you add a new index to a table that is already populated with data, you can now use the YSQL [`CREATE INDEX`](../../../api/ysql/commands/ddl_create_index/#semantics) statement to enable building these indexes in an online manner, without requiring downtime. For details how online backfill of indexes works, see the [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md) design document.
* Backfilling an index while online is disable by default. To enable online index backfilling, set the `yb-tserver` [`--ysql_disable_index_backfill`](../../../reference/configuration/yb-tserver/#ysql-disable-index-backfill) flag to `false` when starting YB-TServers. Note: Do not use this flag in a production cluster yet. For details on how this works, see [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md)

#### Colocated tables

Database-level colocation for YSQL, which started as a beta feature in the 2.1 release, is **now generally available** in the 2.2 release. Traditional RDBMS modeling of parent-child relationships as foreign key constraints can lead to high-latency `JOIN` queries in a geo-distributed SQL database. This is because the tablets (or shards) containing the child rows might be hosted in nodes, availability zones, and regions different from the tablets containing the parent rows. By using [colocated tables](../../../architecture/docdb-sharding/colocated-tables), you can let a single tablet be shared across all tables. Colocation can also be at the overall database level, where all tables of a single database are located in the same tablet and managed by the same Raft group.  Note that tables that you do not want to reside in the overall database’s tablet because of the expectation of large data volume can override the feature at table creation time and hence get independent tablets for themselves.

What happens when the “colocation” tablet containing all the tables of a database becomes too large and starts impacting performance? Check out [automatic tablet splitting [BETA]](#automatic-tablet-splitting-beta).

#### Deferred constraints on foreign keys

Foreign keys in YSQL now support the [DEFERRABLE INITIALLY IMMEDIATE](../../../api/ysql/commands/ddl_create_table/#foreign-key) and [DEFERRABLE INITIALLY DEFERRED](../../../api/ysql/commands/ddl_create_table/#foreign-key) clauses. Work on deferring additional constraints, including those for primary keys, is in progress.

Application developers often declare constraints that their data must obey, leaving it to relational databases to enforce the rules. The end result is simpler application logic, lower error probability, and higher developer productivity. Automatic constraint enforcement is a powerful feature that should be leveraged whenever possible. There are times, however, when you need to temporarily defer enforcement. An example is during the data load of a relational schema where there are cyclic foreign key dependencies. Data migration tools usually defer the enforcement of foreign key dependencies to the end of a transaction by which data for all foreign keys would ideally be present.  This should also allow YSQL to power Django apps.

#### yugabyted for single-node clusters

The `yugabyted` server is now out of beta for single-node deployments. New users can start using YugabyteDB without needing to understand the underlying architectures acts as a parent server, reducing the need to understand data management by YB-TServers and metadata management by YB-Masters.

See it in action by following the updated [Quick start](../../../quick-start). For details, see [`yugabyted`](../../../reference/configuration/yugabyted) in the Reference section.

#### Automatic tablet splitting [BETA]

* YugabyteDB now supports automatic tablet splitting, resharding your data by changing the number of tablets at runtime. For details, see [Tablet splitting](../../../architecture/docdb-sharding/tablet-splitting) and [Automatic Resharding of Data with Tablet Splitting](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md). [#1004](https://github.com/yugabyte/yugabyte-db/issues/1004), [#1461](https://github.com/yugabyte/yugabyte-db/issues/1461), and [#1462](https://github.com/yugabyte/yugabyte-db/issues/1462)
* To enable automatic tablet splitting, use the new `yb-master` [`--tablet_split_size_threshold_bytes`](../../../reference/configuration/yb-master/#tablet-split-size-threshold-bytes) flag to specify the size when tablets should split.

#### TPC-C benchmarking

New results are now available for benchmarking the performance of the YSQL API using the TPC-C suite. For the new TPC-C results and details on performing your own benchmark tests to evaluate YugabyteDB, see [TPC-C](../../benchmark/tpcc-ysql/).

### YCQL

#### Transactional distributed backups

YugabyteDB supports [distributed backup and restore of YCQL databases and tables](../../../manage/backup-restore/snapshot-ysql/). [#1139](https://github.com/yugabyte/yugabyte-db/issues/1139) and [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)

#### Online index backfills

* YugabyteDB can now build indexes on non-empty tables while online, without failing other concurrent writes. When you add a new index to a table that is already populated with data, you can now use the YCQL [CREATE INDEX](../../../api/ycql/ddl_create_index/#synopsis) statement to enable building these indexes in an online manner, without requiring downtime. For details how online backfill of indexes works, see the [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md) design document.
* In YCQL, backfilling an index while online is enabled by default. To disable, set the `yb-tserver` [`--ycql_disable_index_backfill`](../../../reference/configuration/yb-tserver/#ycql-disable-index-backfill) flag to `false` when starting YB-TServers. For details on how online index backfill works, see [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md). [#2301](https://github.com/yugabyte/yugabyte-db/issues/2301) and [#4708](https://github.com/yugabyte/yugabyte-db/issues/4708)

#### Online schema changes for YCQL [BETA]

Most applications have a need to frequently evolve the database schema, while simultaneously ensuring zero downtime during those schema change operations. Therefore, there is a need for schema migrations (which involve DDL operations) to be safely run in a concurrent and online manner alongside foreground client operations. In case of a failure, the schema change should be rolled back and not leave the database in a partially modified state. With the 2.2 release, not only the overall DocDB framework for supporting such schema changes in an online and safe manner has been introduced but also this feature is now available in beta in the context of YCQL using the [`ALTER TABLE`](../../../api/ycql/ddl_alter_table/) statement.

#### Automatic tablet splitting [BETA]

* YugabyteDB now supports automatic tablet splitting at runtime by changing the number of tablets based on size thresholds. For details, see [Tablet splitting](../../../architecture/docdb-sharding/tablet-splitting) and [Automatic Resharding of Data with Tablet Splitting](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md). [#1004](https://github.com/yugabyte/yugabyte-db/issues/1004), [#1461](https://github.com/yugabyte/yugabyte-db/issues/1461), and [#1462](https://github.com/yugabyte/yugabyte-db/issues/1462)
* To enable automatic tablet splitting, use the new `yb-master` [`--tablet_split_size_threshold_bytes`](../../../reference/configuration/yb-master/#tablet-split-size-threshold-bytes) flag to specify the size when tablets should split.

---

## Release notes

### v2.2.3 – September 30, 2020

**Build:** `2.2.3.0-b35`

#### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.2.3.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.2.3.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.2.3.0-b35
```

#### New features

##### Yugabyte Platform

* Enhancements to RBAC with a new “Backup Admin” role which allows all read-only permissions plus back up operations [#5311](https://github.com/yugabyte/yugabyte-db/issues/5311)
* Display table sizes in the Tables page [#3581](https://github.com/yugabyte/yugabyte-db/issues/3581)
* On-prem Cloud provider instances table list can be sorted and filtered [#4757](https://github.com/yugabyte/yugabyte-db/issues/4757)
* API for disabling auto-generation of database nodes certificate [#5545](https://github.com/yugabyte/yugabyte-db/issues/5545)
* Backup retention policy for auto-deletion of older backups [#4493](https://github.com/yugabyte/yugabyte-db/issues/4493)

##### Core database

* [YCQL] Introducing Audit Logging feature for YCQL operations [#5887](https://github.com/yugabyte/yugabyte-db/issues/5887)
* [YSQL] Support for `ALTER COLUMN` type that does not require on-disk changes [#4424](https://github.com/yugabyte/yugabyte-db/issues/4424)
* [YSQL] Support for Transactional batch size in `COPY FROM` command syntax [#2855](https://github.com/yugabyte/yugabyte-db/issues/2855), [#5453](https://github.com/yugabyte/yugabyte-db/issues/5453)
* [YCQL] Finer grained permission control for `TRUNCATE` [#5443](https://github.com/yugabyte/yugabyte-db/issues/5443)
* [YSQL] Enable 2DC replication bootstrap for YSQL Tables [#5601](https://github.com/yugabyte/yugabyte-db/issues/5601)

#### Improvements

##### Yugabyte Platform

* Backup improvements
  * YCQL, YSQL, and YEDIS backups are split into different tabs on backup UI screen
  * Back up operation creates a single restore object regardless of “transactional” field selection
  * Added a new field parallel thread for multi-threading backups
  * YSQL backups only support namespace level backup
  * Backup table list has been revamped with the new UX and shows more details - expiration time, duration, backup ty
  * Universe can be restored with a single action
  * Backup objects can be manually deleted
  * Scheduled backups `cron` expression shows the next run time in local browser time and the label displays that `cron` supports UTC only [#4709](https://github.com/yugabyte/yugabyte-db/issues/4709)
* Improve the Replication graph for xDC replication set up to display the graph and metric names cleanly [#5429](https://github.com/yugabyte/yugabyte-db/issues/5429)
* UI improvements for displaying On-prem instances
  * Instance ID is now optional and also shown in the nodes page [#4760](https://github.com/yugabyte/yugabyte-dbissues/4760)
  * “In Use” column has been renamed to “Universe Name”
  * Supports adding multiple instances in different rows instead of comma-separated entries 
* Supports generating API tokens for SSO-enabled users
* Yugabyte Platform restart cancels any tasks that were in progress
* Remove `sudo` requirement in DB backup script [#5440](https://github.com/yugabyte/yugabyte-db/issues/5440)

##### Core database

* [DocDB] Removal of unreachable (dead) nodes from YugabyteDB UI [#4759](https://github.com/yugabyte/yugabyte-db/issues/4759)
* [DocDB] Improvements with selecting better default flags for Index backfill [#5494](https://github.com/yugabyte/yugabyte-db/issues/5494)
* [DocDB] Lower the leader load balancer parallelism for performance improvements [#5461](https://github.com/yugabyte/yugabyte-db/issues/5461)

#### Bug fixes

##### Yugabyte Platform

* [Platform] Security hardening: Added CSRF token support
* [Platform] Support for PingFederate OIDC SSO login for Non Replicated deployments
* [Platform] Allow on-prem nodes to be reused after releasing them from a universe [#5703](https://github.com/yugabyte/yugabyte-db/issues/5703)

##### Core Database

* [YCQL] Fix for CQL Index scans when `ORDER BY` columns exist in table but not in chosen index [#5690](https://github.com/yugabyte/yugabyte-db/issues/5690)
* [YSQL] Avoids Pushdown down of `UPDATE` with `RETURNING` clause [#5366](https://github.com/yugabyte/yugabyte-db/issues/5366)
* [YSQL] Fix for UPDATE operation with partial and expression Indexes [#4939](https://github.com/yugabyte/yugabyte-db/issues/4939)
* [YSQL] OOM issue fix in `COPY FROM` query [#2855](https://github.com/yugabyte/yugabyte-db/issues/2855)
* [YSQL] OOM issue fix in `COPY TO` query [#5205](https://github.com/yugabyte/yugabyte-db/issues/5205)
* [DocDB] Crash fix for DNS-enabled Yugabyte Universes [#5561](https://github.com/yugabyte/yugabyte-db/issues/5561)
* [DocDB] Fix for in-memory state not getting updated correctly when using read-from-follower query semantics [#1052](https://github.com/yugabyte/yugabyte-db/issues/1052)
* [DocDB] For Yugabyte Universes using cross-cluster async replication (2DC), added RPC throttling on idle CDC tables to avoid wasting CPU [#5472](https://github.com/yugabyte/yugabyte-db/issues/5472)
* [DocDB] Fix for rare deadlock scenario when the node switches from Leader to Follower state of the tablet and there are pending operations on Leader side [#5741](https://github.com/yugabyte/yugabyte-db/issues/5741)
* [DocDB] Fix to ensure all Yugabyte Tablet Servers are accounted for when Master fails over [#5501](https://github.com/yugabyte/yugabyte-db/issues/5501)

#### Known Issues

##### Yugabyte Platform

* Universe disk usage shows up empty on the universe page [#5548](https://github.com/yugabyte/yugabyte-db/issues/5548)

#### Platform Support Package Improvements (Only applicable to Non-Replicated installations)

##### Yugabyte Platform

* Platform backup script needs to preserve attributes on copy from Prometheus snapshot [#5612](https://github.com/yugabyte/yugabyte-db/issues/5612)
* Platform backup fails if `/opt/yugabyte/release` directory does not exist [#5615](https://github.com/yugabyte/yugabyte-db/issues/5615)
* Platform backup script needs to stop Prometheus service during the restore of Prometheus data [#5685](https://github.com/yugabyte/yugabyte-db/issues/5685)

---

### v2.2.2 - August 19, 2020

#### New features

#### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.2.2.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.2.2.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.2.2.0-b15
```

##### Core database

* Fix failed backup if restored table was deleted before restoration. [#5274](https://github.com/yugabyte/yugabyte-db/issues/5274)
* Newly elected YB-Master leader should pause before initiating load balancing. [#5221](https://github.com/yugabyte/yugabyte-db/issues/5221)
* Fix backfilling to better handle large indexed table tablets. [#5031](https://github.com/yugabyte/yugabyte-db/issues/5031)
* Fix `DROP DATABASE` statement should work with databases deleted on YB-Master. [#4710](https://github.com/yugabyte/yugabyte-db/issues/4710)
* For non-prepared statements, optimize `pg_statistic` system table lookups. [#5051](https://github.com/yugabyte/yugabyte-db/issues/5051)
* [CDC] Avoid periodic querying of the `cdc_state` table for xDC metrics if there are no replication streams enabled. [#5173](https://github.com/yugabyte/yugabyte-db/issues/5173)

#### Improvements

* Implement DNS cache to significantly reduce CPU loads due to a large number of DNS resolution requests (especially for YCQL connections). Adds [`dns_cache_expiration_ms`](../../../reference/configuration/yb-master/#dns-cache-expiration-ms) flag (default is 1 minute). [#5201](https://github.com/yugabyte/yugabyte-db/issues/5201)
* Fixed incorrect names de-mangling in index creation from `CatalogManager::ImportSnapshot()`. [#5157](https://github.com/yugabyte/yugabyte-db/issues/5157)
* Fixed crashes when inserting literals containing newline characters. [#5270](https://github.com/yugabyte/yugabyte-db/issues/5270)
* Reuse CQL parser between processors to improve memory usage. Add new `cql_processors_limit` flag to control processor allocation. [#5057](https://github.com/yugabyte/yugabyte-db/issues/5057)

* Backup improvements
  * YCQL, YSQL, and YEDIS backups are split into different tabs on backup UI screen
  * Back up operation creates a single restore object regardless of “transactional” field selection
  * Added a new field parallel thread for multi-threading backups
  * YSQL backups only support namespace level backup
  * Backup table list has been revamped with the new UX and shows more details - expiration time, duration, backup ty
  * Universe can be restored with a single action
  * Backup objects can be manually deleted
  * Scheduled backups `cron` expression shows the next run time in local browser time and the label displays that `cron` supports UTC only [#4709](https://github.com/yugabyte/yugabyte-db/issues/4709)
* Improve the Replication graph for xDC replication set up to display the graph and metric names cleanly [#5429](https://github.com/yugabyte/yugabyte-db/issues/5429)
* UI improvements for displaying On-prem instances
  * Instance ID is now optional and also shown in the nodes page [#4760](https://github.com/yugabyte/yugabyte-dbissues/4760)
  * “In Use” column has been renamed to “Universe Name”
  * Supports adding multiple instances in different rows instead of comma-separated entries 
* Supports generating API tokens for SSO-enabled users
* Yugabyte Platform restart cancels any tasks that were in progress
* Remove `sudo` requirement in DB backup script [#5440](https://github.com/yugabyte/yugabyte-db/issues/5440)

* Fix `yugabyted` fails to start UI due to class binding failure. [#5069](https://github.com/yugabyte/yugabyte-db/issues/5069)
* Show hostnames in YB-Master and YB-TServer Admin UI when hostnames are specified in `--webserver_interface`, `rpc_bind_addresses`, and `server_broadcast_addresses` flags. [#5002](https://github.com/yugabyte/yugabyte-db/issues/5002)
* Skip tablets without intents during commits, for example, the case of an update of a non-existing row. [#5321](https://github.com/yugabyte/yugabyte-db/issues/5321)
* Fix log spew when applying unknown transaction and release a mutex as soon as possible when transaction is not found. [#5315](https://github.com/yugabyte/yugabyte-db/issues/5315)
* Fix snapshots cleanup when snapshots removed before restart. [#5337](https://github.com/yugabyte/yugabyte-db/issues/5337)
* Replace `SCHECK` with `LOG(DFATAL)` when checking for restart-safe timestamps in WAL entries. [#5314](https://github.com/yugabyte/yugabyte-db/issues/5314)
* Replace all CHECK in the load balancer code with `SCHECK` or `RETURN_NOT_OK`. [#5182](https://github.com/yugabyte/yugabyte-db/issues/5182)
* Implement DNS cache to significantly reduce CPU loads due to a large number of DNS resolution requests (especially for YCQL connections). Adds YB-Master [`dns_cache_expiration_ms`](../../../reference/configuration/yb-master/#dns-cache-expiration-ms) flag (default is 1 minute). [#5201](https://github.com/yugabyte/yugabyte-db/issues/5201)
* Avoid duplicate DNS requests when handling `system.partitions` table requests. [#5225](https://github.com/yugabyte/yugabyte-db/issues/5225)
* Fix leader load balancing can cause `CHECK` failures if stepdown task is pending on next run. Sets global leader balance threshold while allowing progress to be made across tables. Adds new YB-Master [`load_balancer_max_concurrent_moves_per_table`](../../../reference/configuration/yb-master/#load-balancer-max-concurrent-moves-per-table) flag to limit number of leader moves per table. [#5181](https://github.com/yugabyte/yugabyte-db/issues/5181) and [#5021](https://github.com/yugabyte/yugabyte-db/issues/5021)
* Set the async `YBClient` initialization future in `TabletPeer` constructor to ensure tablet bootstrap logic can resolve transaction statuses. [#5215](https://github.com/yugabyte/yugabyte-db/issues/5215)
* Handle write operation failures during tablet bootstrap. [#5224](https://github.com/yugabyte/yugabyte-db/issues/5224)
* Drop index upon failed backfill. [#5144](https://github.com/yugabyte/yugabyte-db/issues/5144)  [#5161](https://github.com/yugabyte/yugabyte-db/issues/5161)
* Fix WAL overwriting by new leader and replay of incorrect entries on tablet bootstrap. [#5003](https://github.com/yugabyte/yugabyte-db/issues/5003) [#3759](https://github.com/yugabyte/yugabyte-db/issues/3759) [#4983](https://github.com/yugabyte/yugabyte-db/issues/4983)
* Avoid taking unique lock when starting lookup request. [#5059](https://github.com/yugabyte/yugabyte-db/issues/5059)
* Set 2DC lag metrics to `0` if not the leader and if the replication is deleted. [#5113](https://github.com/yugabyte/yugabyte-db/issues/5113)
* Set the table to ALTERING state when `fully_*` is populated. [#5139](https://github.com/yugabyte/yugabyte-db/issues/5139)
* `Not the leader` errors should not cause a replica to be marked as failed. [#5072](https://github.com/yugabyte/yugabyte-db/issues/5072)
* Use difference between follower's hybrid time and its safe time as a measure of staleness. [#4868](https://github.com/yugabyte/yugabyte-db/issues/4868)

* [DocDB] Removal of unreachable (dead) nodes from YugabyteDB UI [#4759](https://github.com/yugabyte/yugabyte-db/issues/4759)
* [DocDB] Improvements with selecting better default flags for Index backfill [#5494](https://github.com/yugabyte/yugabyte-db/issues/5494)
* [DocDB] Lower the leader load balancer parallelism for performance improvements [#5461](https://github.com/yugabyte/yugabyte-db/issues/5461)

#### Bug fixes

##### Yugabyte Platform

* [Platform] Security hardening: Added CSRF token support
* [Platform] Support for PingFederate OIDC SSO login for Non Replicated deployments
* [Platform] Allow on-prem nodes to be reused after releasing them from a universe [#5703](https://github.com/yugabyte/yugabyte-db/issues/5703)

##### Core Database

* [YCQL] Fix for CQL Index scans when `ORDER BY` columns exist in table but not in chosen index [#5690](https://github.com/yugabyte/yugabyte-db/issues/5690)
* [YSQL] Avoids Pushdown down of `UPDATE` with `RETURNING` clause [#5366](https://github.com/yugabyte/yugabyte-db/issues/5366)
* [YSQL] Fix for UPDATE operation with partial and expression Indexes [#4939](https://github.com/yugabyte/yugabyte-db/issues/4939)
* [YSQL] OOM issue fix in `COPY FROM` query [#2855](https://github.com/yugabyte/yugabyte-db/issues/2855)
* [YSQL] OOM issue fix in `COPY TO` query [#5205](https://github.com/yugabyte/yugabyte-db/issues/5205)
* [DocDB] Crash fix for DNS-enabled Yugabyte Universes [#5561](https://github.com/yugabyte/yugabyte-db/issues/5561)
* [DocDB] Fix for in-memory state not getting updated correctly when using read-from-follower query semantics [#1052](https://github.com/yugabyte/yugabyte-db/issues/1052)
* [DocDB] For Yugabyte Universes using cross-cluster async replication (2DC), added RPC throttling on idle CDC tables to avoid wasting CPU [#5472](https://github.com/yugabyte/yugabyte-db/issues/5472)
* [DocDB] Fix for rare deadlock scenario when the node switches from Leader to Follower state of the tablet and there are pending operations on Leader side [#5741](https://github.com/yugabyte/yugabyte-db/issues/5741)
* [DocDB] Fix to ensure all Yugabyte Tablet Servers are accounted for when Master fails over [#5501](https://github.com/yugabyte/yugabyte-db/issues/5501)

#### Known Issues

##### Yugabyte Platform

* Universe disk usage shows up empty on the universe page [#5548](https://github.com/yugabyte/yugabyte-db/issues/5548)

* Add **Master** section below **Tablet Server** section in **Metrics** page. [#5233](https://github.com/yugabyte/yugabyte-db/issues/5233)
* Add `rpc_connections_alive` metrics for YSQL and YCQL APIs. [#5223](https://github.com/yugabyte/yugabyte-db/issues/5223)
* Fix restore payload when renaming table to include keyspace. Disable keyspace field when restoring universe backup. [#5178](https://github.com/yugabyte/yugabyte-db/issues/5178)
* Pass in `ssh_user` to air-gap provision script and add to on-premise template. [#5132](https://github.com/yugabyte/yugabyte-db/issues/5132)
* Add the `--recursive` flag to AZCopy for multi-table restore. [#5163](https://github.com/yugabyte/yugabyte-db/issues/5163)
* Fix transactional backup with specified tables list by including keyspace in payload. [#5149](https://github.com/yugabyte/yugabyte-db/issues/5149)
* Fix undefine object property error when Prometheus is unavailable and navigating to the **Replication** tab. [#5146](https://github.com/yugabyte/yugabyte-db/issues/5146)
* Backups should take provider-level environment variables, including home directory. [#5064](https://github.com/yugabyte/yugabyte-db/issues/5064)
* Add hostname and display in the Nodes page along with node name and IP address. [#4760](https://github.com/yugabyte/yugabyte-db/issues/4760)
* Support option of DNS names instead of IP addresses for nodes. Add option in Admin UI to choose between using hostnames or IP addresses for on-premises provider. [#4951](https://github.com/yugabyte/yugabyte-db/issues/4951) [#4950](https://github.com/yugabyte/yugabyte-db/issues/4950)
* Disable glob before running cleanup of old log files using `zip_purge_yb_logs.sh`. Fixes issue on Red Hat. [#5169](https://github.com/yugabyte/yugabyte-db/issues/5169)
* Fix Replication graph units and missing graph in **Replication** tab when metrics exist. [#5423](https://github.com/yugabyte/yugabyte-db/issues/5423)

##### Yugabyte Platform

* Platform backup script needs to preserve attributes on copy from Prometheus snapshot [#5612](https://github.com/yugabyte/yugabyte-db/issues/5612)
* Platform backup fails if `/opt/yugabyte/release` directory does not exist [#5615](https://github.com/yugabyte/yugabyte-db/issues/5615)
* Platform backup script needs to stop Prometheus service during the restore of Prometheus data [#5685](https://github.com/yugabyte/yugabyte-db/issues/5685)

#### Downloads

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

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.2.0.0-b80
```

#### YSQL

* Support presplitting using [CREATE INDEX...SPLIT INTO](../../../api/ysql/commands/ddl_create_index/#split-into) for range-partitioned table indexes. For details, see [Presplitting](../../../architecture/docdb-sharding/tablet-splitting/#presplitting). [#4235](https://github.com/yugabyte/yugabyte-db/issues/4235)
* Fix crash for nested `SELECT` statements that involve null pushdown on system tables. [#4685](https://github.com/yugabyte/yugabyte-db/issues/4685)
* Fix wrong sorting order in presplit tables. [#4651](https://github.com/yugabyte/yugabyte-db/issues/4651)
* To help track down unoptimized (or "slow") queries, use the new `yb-tserver` [`--ysql_log_min_duration_statement`](../../../reference/configuration/yb-tserver/#ysql-log-min-duration-statement). [#4817](https://github.com/yugabyte/yugabyte-db/issues/4817)
* Enhance the [`yb-admin list_tables`](../../../admin/yb-admin/#list-tables) command with optional flags for listing tables with database type (`include_db_type`), table ID (`include_table_id`), and table type (`include_table_type`). This command replaces the deprecated `yb-admin list_tables_with_db_types` command. [#4546](https://github.com/yugabyte/yugabyte-db/issues/4546)
* Add support for [`ALTER TABLE`](../../../api/ysql/commands/ddl_alter_table/#) on colocated tables. [#4293](https://github.com/yugabyte/yugabyte-db/issues/4293)
* Improve logic for index delete permissions. [#4980](https://github.com/yugabyte/yugabyte-db/issues/4980)
* Add fast path option for index backfill when certain statements can create indexes without unnecessary overhead from online schema migration (for example, `CREATE TABLE` with unique column constraint). Skip index backfill for unsupported statements, including `DROP INDEX`, "CREATE UNIQUE INDEX ON`, and index create in postgres nested DDL). [#4918]
* Suppress `incomplete startup packet` messages in YSQL logs. [#4813](https://github.com/yugabyte/yugabyte-db/issues/4813)
* Improve YSQL create namespace failure handling. [#3979](https://github.com/yugabyte/yugabyte-db/issues/3979)
* Add error messages to table schema version mismatch errors so they are more understandable. [#4810]
* Increase the default DDL operations timeout to 120 seconds to allow for multi-region deployments, where a CREATE DATABASE statement can take longer than one minute. [#4762](https://github.com/yugabyte/yugabyte-db/issues/4762)

#### YCQL

* Throttle YCQL calls when soft memory limit is reached. Two new flags, `throttle_cql_calls_on_soft_memory_limit` and `throttle_cql_calls_policy` can be used to control it. [#4973](https://github.com/yugabyte/yugabyte-db/issues/4973)
* Implements a password cache to allow connections to be created more quickly from recently used accounts. Helps reduce high CPU usage when using YCQL authorization. [#4596](https://github.com/yugabyte/yugabyte-db/issues/4596)
* Fix `column doesn't exist` error when an index is created on a column which is a prefix of another column. [#4881](https://github.com/yugabyte/yugabyte-db/issues/4881)
* Fix crashes when using ORDER BY for non-existent column with index scan. [#4908](https://github.com/yugabyte/yugabyte-db/issues/4908)

#### Core database

* Add HTTP endpoints for determining master leadership and returning information on masters. [#2606](https://github.com/yugabyte/yugabyte-db/issues/2606)
  * `<web>/api/v1/masters`: Returns all master statuses.
  * `<web>/api/v1/is-leader`: Returns `200 OK` response status code when the master is a leader and `503 Service Unavailable` when the master is not a leader.
* Add HTTP endpoint `/api/v1/cluster-config` for YB-Master to return the current cluster configuration in JSON format. [#4748](https://github.com/yugabyte/yugabyte-db/issues/4748)
  * Thanks, [AbdallahKhaled93](https://github.com/AbdallahKhaled93), for your contribution!
* Output of `yb-admin get_universe_config` command is now in JSON format for easier parsing. [#4589][#1462](https://github.com/yugabyte/yugabyte-db/issues/4589)
* Add `yugabyted destroy` command. [#3872][#3849](https://github.com/yugabyte/yugabyte-db/issues/3872)
* Change logic used to determine if the load balancer is idle. [#4707](https://github.com/yugabyte/yugabyte-db/issues/4707)
* Default to IPv4 addresses for DNS resolution and local addresses. [#4851](https://github.com/yugabyte/yugabyte-db/issues/4293)
* Add a Grafana dashboard for YugabyteDB. [#4725](https://github.com/yugabyte/yugabyte-db/issues/4725)
* [CDC] Check for table properties equivalence when comparing schemas in 2DC setups and ignore properties that don't need to be the same. [#4233](https://github.com/yugabyte/yugabyte-db/issues/4233)
* [DocDB] Allow multiple indexes to backfill or delete simultaneously. [#2784](https://github.com/yugabyte/yugabyte-db/issues/2784)
* [DocDB] Transition to new leader gracefully during a leader stepdown. When a leader stepdown happens with no new leader candidate specified in the stepdown request, the peer simply steps down leaving the group with no leader until regular heartbeat timeouts are triggered. This change makes it so that the leader attempts to transition to the most up-to-date peer, if possible. [#4298](https://github.com/yugabyte/yugabyte-db/issues/4298)
* Update [`yb-admin list_snapshots`](../../../admin/yb-admin/#list-snapshots) command to use `not_show_restored` option to exclude fully RESTORED entries. [#4351](ttps://github.com/yugabyte/yugabyte-db/issues/4351)
* [DocDB] Clean up leftover snapshot files from failed snapshots that resulted in remote bootstrap getting stuck in a failure loop. [#4745](https://github.com/yugabyte/yugabyte-db/issues/4745)
* [DocDB] Clean up metadata in memory after deleting snapshots. [#4887](https://github.com/yugabyte/yugabyte-db/issues/4877)
* [DocDB] Fix snapshots getting stuck in retry loop with deleted table. [#4610](https://github.com/yugabyte/yugabyte-db/issues/4877) and [#4302](https://github.com/yugabyte/yugabyte-db/issues/4302)
* [DocDB] When using [`yb-admin master_leader_stepdown`](../../../admin/yb-admin/#master-leader-stepdown), specifying a new leader is now optional. [#4722](https://github.com/yugabyte/yugabyte-db/issues/4722)
* [DocDB] Add breakdown of disk space on TServer dashboard. [#4767](https://github.com/yugabyte/yugabyte-db/issues/4767)
* [DocDB] Suppress `yb-admin` from logging harmless "Failed to schedule invoking callback on response" warning. [#4131](https://github.com/yugabyte/yugabyte-db/issues/4131)
* [DocDB] Allow specifying IPv6 addresses in bind addresses. [#3644](https://github.com/yugabyte/yugabyte-db/issues/3644)
* [DocDB] Fix TS Heartbeater can get stuck if master network connectivity breaks. [#4838](https://github.com/yugabyte/yugabyte-db/issues/4838)
* [DocDB] Improve protection against hitting hard memory limit when follower tablet peers are lagging behind leaders on a node. (for example, node downtime because of yb-tserver restart). [#2563](https://github.com/yugabyte/yugabyte-db/issues/2563)
* `yugabyted` `--bind_ip` flag renamed to `--listen`. [#4960](https://github.com/yugabyte/yugabyte-db/issues/4960)
* Fix malformed URL for ycql rpcz when clicking **YCQL Live Ops** link in the YB-TServer utilities page. [#4886](https://github.com/yugabyte/yugabyte-db/issues/4866)
* Fix export name for YCQL metrics. [#4955](https://github.com/yugabyte/yugabyte-db/issues/4955)
* Fix YB-Master UI to show correct number of tablets for colocated tables. [#4699](https://github.com/yugabyte/yugabyte-db/issues/4699)

#### Yugabyte Platform

* Add option to [back up and restore YSQL databases](../../../manage/backup-restore/snapshot-ysql/) and universe-level transactional backups. [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)
* On the **Universes** page, the **Replication** tab displays by default and adds default state when replication lag metric query returns no data.
* Add **Replication** tab to Universe overview tab list if enabled and query for replication lag metrics. [#]
* Add replication lag metrics `async_replication_sent_lag_micros` (last applied time on producer - last polled for record) and `async_replication_committed_lag_micros` (last applied time on producer * last applied time on consumer) for 2DC replication and export to Prometheus. [#2154](https://github.com/yugabyte/yugabyte-db/issues/2154)
* Retrieve IAM Instance Profile Credentials for backups. This retrieves the `AccessKeyId` and `SecretAccessKey` and set these in the configuration so that data nodes inherit the required IAM permissions to access S3. [#3451](https://github.com/yugabyte/yugabyte-db/issues/3451) and [#4900](https://github.com/yugabyte/yugabyte-db/issues/4900)
* Add option to create a backup of multiple YCQL transactional tables. [#4540](https://github.com/yugabyte/yugabyte-db/issues/4540)
* Add support for c5d instance types. [#4914](https://github.com/yugabyte/yugabyte-db/issues/4914)
* Add support for installing epel-release in Amazon Linux. [#4561](https://github.com/yugabyte/yugabyte-db/issues/4561)
* Fixed requested TLS client certificate generated with expired date and ysqlsh fails to connect. [#4732](https://github.com/yugabyte/yugabyte-db/issues/4732)
* Fixed universe fails to create if user-supplied certificate is selected. [#4733](https://github.com/yugabyte/yugabyte-db/issues/4733)
* Implement OAuth2/SSO authentication for Yugabyte Platform sign-in. [#4633](https://github.com/yugabyte/yugabyte-db/issues/4644) and [#4420](https://github.com/yugabyte/yugabyte-db/issues/4420)
* Add backup and restore options for YSQL tables and universe-level transactional backups. [#3849](https://github.com/yugabyte/yugabyte-db/issues/3849)
* Retrieve IAM Instance Profile Credentials for backups. This retrieves the `AccessKeyId` and `SecretAccessKey` and set these in the configuration so that data nodes inherit the required IAM permissions to access S3. [#4900](https://github.com/yugabyte/yugabyte-db/issues/4900)
* In the **Health and Alerting** tab of the Admin Console, the Username and Password fields in **Custom SMTP Configuration** are now optional. [#4952](https://github.com/yugabyte/yugabyte-db/issues/4952)
* Fix password reset in customer profile page. [#4666](https://github.com/yugabyte/yugabyte-db/issues/4666) and [#3909](https://github.com/yugabyte/yugabyte-db/issues/3909)
* Fix **Backups** page not loading when there is a pending task. [#4754](https://github.com/yugabyte/yugabyte-db/issues/4754)
* Change UI text displaying "GFlag" to "Flag" in the Admin Console. [#4659](https://github.com/yugabyte/yugabyte-db/issues/4659)
