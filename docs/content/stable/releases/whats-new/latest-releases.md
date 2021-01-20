---
title: What's new in the v2.3 latest release series
headerTitle: What's new in the v2.3 latest release series
linkTitle: 2.3 (latest)
description: Enhancements, changes, and resolved issues in the latest release series.
headcontent: Features, enhancements, and resolved issues in the latest release series.
image: /images/section_icons/quick_start/install.png
section: RELEASES
menu:
  stable:
    parent: whats-new
    identifier: latest-releases
    weight: 2585
isTocNested: true
showAsideToc: true 
---

Included here are the release notes for all releases in the v2.3 latest release series.

{{< note title="New release versioning" >}}

Starting with v2.2, Yugabyte release versions follow a [new release versioning convention](../../versioning). The latest release series, denoted by `MAJOR.ODD`, incrementally introduces new features and changes and is intended for development and testing only. Revision releases, denoted by `MAJOR.ODD.REVISION` versioning, can include new features and changes that might break backwards compatibility. For more information, see [Supported and planned releases](../../releases-overview).

{{< /note >}}

{{< note title="Upgrading from 1.3" >}}

Prior to v2.0, YSQL was still in beta. Upon release of v2.0, a backward-incompatible file format change was made for YSQL. For existing clusters running pre-2.0 release with YSQL enabled, you cannot upgrade to v2.0 or later. Instead, export your data from existing clusters and then import the data into a new cluster (v2.0 or later).

{{< /note >}}

## Latest release: v2.3.3 - October 26, 2020

**Build:** `2.3.3.0-b106`

### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.3.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.3.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

### Docker

```sh
docker pull yugabytedb/yugabyte:2.3.3.0-b106
```

## Notable features and changes (cumulative for the v2.3 latest release series)

Note: Content will be added as new notable features and changes are available in the patch releases of the v2.3 latest release series. For the latest v2.3 release notes, see [Release notes](#release-notes) below.

### YSQL

- Load data from a large file to a table using the improved `COPY <table> FROM <file>` statement. For details, see [`COPY`](../../../api/ysql/the-sql-language/statements/cmd_copy/). Improved memory management should prevent out-of-memory (OOM) issues.

- Specify smaller transaction sizes using the `COPY OPTION` `ROWS_PER_TRANSACTION`. For an example, see [Import a large table using smaller transactions](../../../api/ysql/the-sql-language/statements/cmd_copy/#import-a-large-table-using-smaller-transactions).

## Release notes

### v2.3.3 - October 26, 2020

**Build:** `2.3.3.0-b106`

#### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.3.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.3.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.3.3.0-b106
```

#### New features

##### Yugabyte Platform

* Introducing beta support for Azure IaaS orchestration in Yugabyte Platform cloud provider configuration. ([6094](https://github.com/yugabyte/yugabyte-db/issues/6094), [6020](https://github.com/yugabyte/yugabyte-db/issues/6020))
* Yugabyte Platform operations now allow promoting a Yugabyte TServer only node to run YB-Master and YB-TServer process. ([5831](https://github.com/yugabyte/yugabyte-db/issues/5831))

##### Core database

* [YSQL] Introducing support for Event Triggers that are global to a particular database and are capable of capturing DDL events. ([2379](https://github.com/yugabyte/yugabyte-db/issues/2379))

#### Improvements

##### Yugabyte Platform

* Enhancements to on-premises deployment workflows:
  * Do not fail universe creation if cronjobs can't be created for on-premises. ([5939](https://github.com/yugabyte/yugabyte-db/issues/5939))
  * Remove pre-provision script requirement for airgapped installations. ([5929](https://github.com/yugabyte/yugabyte-db/issues/5929))
  * "Sudo passwordless" in on-premises cloud provider configuration toggle is renamed.
  * Allow `yugabyte` user to belong to other user groups in Linux. ([5943](https://github.com/yugabyte/yugabyte-db/issues/5943))
  * Added a new "Advanced" section  in on-prem cloud provider configuration which includes:
    * Use hostnames
    * Desired home directory
    * Node exporter settings
  * Improvements to installation of Prometheus Node Exporter utility workflow. ([5926](https://github.com/yugabyte/yugabyte-db/issues/5926))
    * The node exporter option is now available in the cloud configuration under advanced settings.
    * Supports bringing your own node exporter user.
* Make YEDIS API optional for new Universes and no change in behavior of existing universes. ([5207](https://github.com/yugabyte/yugabyte-db/issues/5207))
* UI/UX improvements for YB Platform:
  * Add visual feedback when backup or restore is initiated from modal. ([5908](https://github.com/yugabyte/yugabyte-db/issues/5908))
  * Minor fixes to primary cluster widget text issue. ([5988](https://github.com/yugabyte/yugabyte-db/issues/5988))
  * Show pre-provision script in UI for non-passwordless sudo on-premises provider. ([5550](https://github.com/yugabyte/yugabyte-db/issues/5550))
  * Update backup target and backup pages. ([5917](https://github.com/yugabyte/yugabyte-db/issues/5917))
* For YugabyteDB universes with replication factor (RF) > 3, change the default `min_num` replicas for even distribution of AZs across all regions. ([5426](https://github.com/yugabyte/yugabyte-db/issues/5426))
* Added functionality to create IPv6-enabled universe in Kubernetes. ([5309](https://github.com/yugabyte/yugabyte-db/issues/5309), [5235](https://github.com/yugabyte/yugabyte-db/issues/5235))

##### Core database

* Support for SQL/JSON Path Language (`jsonb_path_query`). ([5408](https://github.com/yugabyte/yugabyte-db/issues/5408))
* Remove range-loop-analysis and fallthrough errors in our code. ([5812](https://github.com/yugabyte/yugabyte-db/issues/5812))
* Incorrect index update if used expression result is still unchanged. ([5898](https://github.com/yugabyte/yugabyte-db/issues/5898))
* Various improvements to `RaftConsensus` tests. ([4548](https://github.com/yugabyte/yugabyte-db/issues/4548), [5912](https://github.com/yugabyte/yugabyte-db/issues/5912))
* As part of the tablet splitting feature:
  * Implemented cleanup of the tablet for which all replicas have been split for. ([4929](https://github.com/yugabyte/yugabyte-db/issues/4929))
  * Compaction improvements ([5523](https://github.com/yugabyte/yugabyte-db/issues/5523))
* Improve performance for sequences by using higher cache value by default. Controlled by a YB-TServer configuration flag `--ysql_sequence_cache_minval`.  ([6041](https://github.com/yugabyte/yugabyte-db/issues/6041))
* Added compatibility mode in the `yb_backup.py` script for YugabyteDB version earlier than v2.1.4. ([5810](https://github.com/yugabyte/yugabyte-db/issues/5810))
* Stability improvements to core database - make exponential backoff on lagging RAFT followers send NOOP. ([5527](https://github.com/yugabyte/yugabyte-db/issues/5527))
* Added use of separate metrics objects for `RegularDB` and `IntentsDB`. ([5640](https://github.com/yugabyte/yugabyte-db/issues/5640))
* Improvements to 2DC (xCluster replication) by adding TLS support for cleaning up `cdc_state` table. ([5905](https://github.com/yugabyte/yugabyte-db/issues/5905))

#### Bug fixes

##### Yugabyte Platform

* Fix for Universe disk usage shows up empty on the universe page. ([5548](https://github.com/yugabyte/yugabyte-db/issues/5548))
* Fix on on-premises backup failures due to file owned by the root user. ([6062](https://github.com/yugabyte/yugabyte-db/issues/6062))
* Fix for a bug where user operation to perform a change to nodes count by AZ was doing a full move. ([5335](https://github.com/yugabyte/yugabyte-db/issues/5335))
* Fixes for Yugabyte Platform data backup script for Replicated-based installations
* Fixes to Client Certificate start time to use UTC during download. ([6118](https://github.com/yugabyte/yugabyte-db/issues/6118))
* Fixes for migration if no access keys exist yet. ([6099](https://github.com/yugabyte/yugabyte-db/issues/6099))
* Fix to resolve issues caused by starting a YB-TServer node when another YB-Master node is down in the universe. ([5739](https://github.com/yugabyte/yugabyte-db/issues/5739))
* Use the correct disk mount point while calculating disk usage of logs. ([5983](https://github.com/yugabyte/yugabyte-db/issues/5983))
* Fixes to delete backups for TLS-enabled universes. ([5980](https://github.com/yugabyte/yugabyte-db/issues/5980))

##### Core database

* Fix for bug with the duplicate row detection that allows a unique index to get created when the table is not unique on the index columns. ([5811](https://github.com/yugabyte/yugabyte-db/issues/5811))
* Improve fault tolerance of DDLs and reduce version mismatch errors in YSQL. ([3979](https://github.com/yugabyte/yugabyte-db/issues/3979), [4360](https://github.com/yugabyte/yugabyte-db/issues/4360))
* Fixes to incorrect column-ids in the restored table if the original table was altered. ([5958](https://github.com/yugabyte/yugabyte-db/issues/5958))
* Fixes timeout bug in Yugabyte Platform when there are read replicas. This fix will ignore read replica YB-TServers when running `AreLeadersOnPreferredOnly`. ([6081](https://github.com/yugabyte/yugabyte-db/issues/6081))
* Fixes to restore of YSQL Backups after dropping and recreating a database. ([5651](https://github.com/yugabyte/yugabyte-db/issues/5651))
* Fixes to a bug with the duplicate row detection that allows a unique index to get created when the table is not unique on the index columns. This is fixed by setting the read time for backfill write requests to be the safe time chosen for backfill read requests rather than the hard-coded time `50`. ([5811](https://github.com/yugabyte/yugabyte-db/issues/5811))

#### Known Issues

##### Yugabyte Platform

* Azure IaaS orchestration:
  * No pricing information provided. ([5624](https://github.com/yugabyte/yugabyte-db/issues/5624))
  * No support for regions with zero availability zones (AZs). ([5628](https://github.com/yugabyte/yugabyte-db/issues/5628))

### v2.3.2 - September 29, 2020

**Build:** `2.3.2.0-b37`

#### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.2.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.2.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.3.2.0-b37
```

#### Features and changes

##### YSQL

- Enable `pg_stat_statements` extension by default. [#5750](https://github.com/yugabyte/yugabyte-db/issues/5750)
- Clean up unused `PgStatement` objects on hot path after usage to avoid memory consumption. [#5374](https://github.com/yugabyte/yugabyte-db/issues/5374)
- Fix restarting writes (`INSERT` and `UPDATE)` on transaction conflicts and avoid surfacing transaction errors to user applications. [#4291](https://github.com/yugabyte/yugabyte-db/issues/4291)
- Enable batching of updates for YSQL procedures and functions. [#5257](https://github.com/yugabyte/yugabyte-db/issues/5257)

##### YCQL

- Add additional system queries (`SELECT_LOCAL`, `SELECT_LOCAL_TOKENS`, and `SELECT_PEERS_TOKENS`) for caching. [#5753](https://github.com/yugabyte/yugabyte-db/issues/5753)
- Fix crash for YCQL index scan when `ORDER BY` columns exist in table, but not in the chosen index, and raise an error. [#5690](https://github.com/yugabyte/yugabyte-db/issues/5690)

##### Core database

- Allow setting a custom replacement policy per table through `yb_admin`. [#5368](https://github.com/yugabyte/yugabyte-db/pull/5368)
- Do not clean up transaction that potentially was committed. [#5789](https://github.com/yugabyte/yugabyte-db/issues/5789) [#5783](https://github.com/yugabyte/yugabyte-db/issues/5783) [#5755](https://github.com/yugabyte/yugabyte-db/issues/5755)
- Fix CDC shutdown races. Moved thread pool shutdown before other CDC objects cleanup. Added explicit CDC poller clear in CDC consumer shutdown. [#5583](https://github.com/yugabyte/yugabyte-db/issues/5583)
- Fix preparer deadlock after switching from leader to follower. [#5741](https://github.com/yugabyte/yugabyte-db/issues/5741)
- Fix `RaftGroupMetadata` construction issues found using PVS Studio. [#5578](https://github.com/yugabyte/yugabyte-db/issues/5578)
- Change `rlimit` flags to have at-least semantics. Only change the process soft limit if it is smaller than the limit specified by the corresponding flag value. [#5715](https://github.com/yugabyte/yugabyte-db/issues/5715)
- Add extra logging when initializing `syscatalog`. [#5708](https://github.com/yugabyte/yugabyte-db/issues/5708)
- Set default `webserver_interface` to `rpc_bind_address` IP/host component. [#5320](https://github.com/yugabyte/yugabyte-db/issues/5320)
- Add environmental variables functionality to `yugabyted`. New environment variables for YSQL (`YSQL_USER`, `YSQL_PASSWORD`, and `YSQL_DB`) and YCQL(`YCQL_USER`, `YCQL_PASSWORD`, and `YCQL_KEYSPACE`). ([#5499](https://github.com/yugabyte/yugabyte-db/issues/5499)
- Improve cleanup of aborted transactions. [#5669](https://github.com/yugabyte/yugabyte-db/issues/5669)
- Fix dumping an empty batch by updating MaxReplicateOpId. [#5679](https://github.com/yugabyte/yugabyte-db/issues/5679)
- Set default staleness for read from followers (using `max_stale_read_time_ms`) to 60 seconds. Previously, a follower could return requested data no matter how state the data is. [#5590](https://github.com/yugabyte/yugabyte-db/issues/5590)
- Calculate the placement UUID from Raft upon tserver registration. [#5558](https://github.com/yugabyte/yugabyte-db/issues/5558)
- Enable ignoring deleted tablets. [#5122](https://github.com/yugabyte/yugabyte-db/issues/5122)
- Remove unreachable nodes from UI (`/tablet-servers` page) after nodes are removed from the cluster. [#4759](https://github.com/yugabyte/yugabyte-db/issues/4759)

##### Yugabyte Platform

- Fix issue parsing multiple comma-separated addresses for `rpc_bind_addresses`. [#5634](https://github.com/yugabyte/yugabyte-db/issues/5634)
- Add functionality to delete old backups after a specified time period. In the **Create Backup** dialog, added **Number of Days to Retain Backup** field. [#4493](https://github.com/yugabyte/yugabyte-db/issues/4493)
- Combine the Platform and community Grafana dashboards. Grafana dashboard shows the node status and other panels correctly. [#5707](https://github.com/yugabyte/yugabyte-db/issues/5707)
- Sum table sizes across all nodes of a universe. [#5745](https://github.com/yugabyte/yugabyte-db/issues/5745)
- Fixes releasing OnPrem nodes should not delete them from the universe metadata. Fixes releasing a node should only attempt to shut down processes on that node. Nodes can be re-added using **Node > Actions > Add** flow. [#5703](https://github.com/yugabyte/yugabyte-db/issues/5703) [#5791](https://github.com/yugabyte/yugabyte-db/issues/5791) [#5792](https://github.com/yugabyte/yugabyte-db/issues/5792)
- Add additional property to MetricsPanel to handle width of Replication graph and increase height. Aggregates replication metrics values from all tservers into an average and max value graph across all tservers. [#5429](https://github.com/yugabyte/yugabyte-db/issues/5429) [#5613](https://github.com/yugabyte/yugabyte-db/issues/5613) [#5632](https://github.com/yugabyte/yugabyte-db/issues/5632)
- Update yb-client JAR version to `0.8.1`. [#5759](https://github.com/yugabyte/yugabyte-db/issues/5759)
- Stop and restart relevant services during Yugabyte Platform backup. Backups stop and restart services. Restores stop and restart Yugabyte Platform and Prometheus, if necessary. [#5685](https://github.com/yugabyte/yugabyte-db/issues/5685)
- Authenticate API tokens even when using OAuth. This allows `curl` requests. [#5587](https://github.com/yugabyte/yugabyte-db/issues/5587)
- Fix Metrics tab and columns misalignment on table page. [#5582](https://github.com/yugabyte/yugabyte-db/issues/5582)
- Rename **In Use** column to **Universe Name** and show `NOT USED` if the node is not in use. [#5713](https://github.com/yugabyte/yugabyte-db/issues/5713)
- Do not generate node certificates for custom certificates. [#5545](https://github.com/yugabyte/yugabyte-db/issues/5545)
- Make instance name field optional for on-premises provider. [#5234](https://github.com/yugabyte/yugabyte-db/issues/5234)
- Fix button for Metrics tab to reset active and focus state when key is unpressed. [#5620](https://github.com/yugabyte/yugabyte-db/issues/5620)
- Add Docker- and VM-aware options to the backup script and will include Prometheus snapshots. [#5120](https://github.com/yugabyte/yugabyte-db/issues/5120) [#5615](https://github.com/yugabyte/yugabyte-db/issues/5615) [#5612](https://github.com/yugabyte/yugabyte-db/issues/5612) [#5614](https://github.com/yugabyte/yugabyte-db/issues/5614)

### v2.3.1 – September 15, 2020

**Build:** `2.3.1.0-b15`

#### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.1.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.1.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.3.1.0-b15
```

#### Features and changes

##### YSQL

- Fixes OOM when using `COPY <table> FROM <file>` to load data from a large file to a table. [#5453](https://github.com/yugabyte/yugabyte-db/issues/5453)
- Enable 2DC replication bootstrap for YSQL tables. This allows the producer to know where to start replicating from. [#5601](https://github.com/yugabyte/yugabyte-db/issues/5601)
- Fix `CREATE TABLE` is 4-5x slower using Docker on Mac than not using Docker. Speeds up table creation by buffering writes to postgres system tables, caching pinned objects, and significantly reducing write RPC calls. [#3503](https://github.com/yugabyte/yugabyte-db/issues/3503)
- Roll back the catalog version made in commit `46f3701` so that 2.3 upgrades can proceed. [#5408](ttps://github.com/yugabyte/yugabyte-db/issues/5408)

##### Core database

- Quickly evict known unresponsive tablet servers from the tablet location cache. Applies only to follower reads. For example, when tablet servers are unresponsive or dead — not sending heartbeats to the master for 5 minutes. This could also happen after decommissioning nodes. [#1052](https://github.com/yugabyte/yugabyte-db/issues/1052)

##### Yugabyte Platform

- Add search input and data sorting to the on-premises instances table list. Click arrows next to column titles to sort. Use the Search form to search multiple columns. [#4757](https://github.com/yugabyte/yugabyte-db/issues/4757)
- Add the ability to change the user role (`Admin`, `ReadOnly`, or `BackupAdmin`) from the UI by an admin. Also, fix stale users list after creation or deletion of a user and disable **Save** buttons at Customer Profile tabs for `ReadOnly` users. [#5311](https://github.com/yugabyte/yugabyte-db/issues/5311)
- Before deleting a universe, stop the master/tserver processes and release the instance so that if started again, the processes are not still running. [#4953](https://github.com/yugabyte/yugabyte-db/issues/4953)

### v2.3.0 – September 8, 2020

**Build:** `2.3.0.0-b176`

#### Downloads

<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.0.0-darwin.tar.gz">
  <button>
    <i class="fab fa-apple"></i><span class="download-text">macOS</span>
  </button>
</a>
&nbsp; &nbsp; &nbsp;
<a class="download-binary-link" href="https://downloads.yugabyte.com/yugabyte-2.3.0.0-linux.tar.gz">
  <button>
    <i class="fab fa-linux"></i><span class="download-text">Linux</span>
  </button>
</a>
<br />

#### Docker

```sh
docker pull yugabytedb/yugabyte:2.3.0.0-b176
```

#### Features and changes

##### YSQL

- Fix OOM when running large `COPY TO` statements by creating new memory context for the loop over retrieved rows and resetting it after processing each row. [#5205](https://github.com/yugabyte/yugabyte-db/issues/5205)
- Support transactional batch size for [`COPY FROM` command](../../../api/ysql/the-sql-language/statements/cmd_copy) with OOM fix. Batch sizes can be passed in with `ROWS_PER_TRANSACTION` in the `COPY OPTION` syntax. For an example, see [Import a large table using smaller transactions](../../../api/ysql/the-sql-language/statements/cmd_copy/#import-a-large-table-using-smaller-transactions) [#2855](https://github.com/yugabyte/yugabyte-db/issues/2855) [#5453](https://github.com/yugabyte/yugabyte-db/issues/5453)
- For index backfill flags, use better default values. Set `index_backfill_rpc_timeout_ms` default from `60000` to `30000` and change `backfill_index_timeout_grace_margin_ms` default from `50` to `500`. [#5494](https://github.com/yugabyte/yugabyte-db/issues/5494)
- Remove spurious error message "0A000: Alter table is not yet supported" from `CREATE OR REPLACE VIEW`. [#5071](https://github.com/yugabyte/yugabyte-db/issues/5071)
- Prevent consistency violations when a partitioned table has foreign key constraints due to erroneous classification as a single-row transaction. [#5387](https://github.com/yugabyte/yugabyte-db/issues/5387)
- Fix restore from a distributed backup fails for tables using `SPLIT INTO` without a primary key. [#4993](https://github.com/yugabyte/yugabyte-db/issues/4993)
- Fix wrong result by avoiding pushdown of `UPDATE` statement with the `RETURNING` clause. [#5366](https://github.com/yugabyte/yugabyte-db/issues/5366)
- Improve error message when `UPDATE` changes partition so its clear what went wrong, fixed `yb_pg_foreign_key` `pg_regress` test for semantic merge conflict when updating primary keys and row-level partitioning. [#659](https://github.com/yugabyte/yugabyte-db/issues/659) [#5179](https://github.com/yugabyte/yugabyte-db/issues/5179) [#5310](https://github.com/yugabyte/yugabyte-db/issues/5310)
- Allow `UPDATE` statement to change primary key columns. [#659](https://github.com/yugabyte/yugabyte-db/issues/659)
- Support row-level partitioning. [#5179](https://github.com/yugabyte/yugabyte-db/issues/5179)
- Block usage of `TABLEGROUP` with the `SPLIT` clause. For `CREATE TABLE`, usage is blocked in the grammar. For `CREATE INDEX`, if `NO TABLEGROUP` was provided, then presplitting for the index is allowed; otherwise, an error is issued. [#5352](https://github.com/yugabyte/yugabyte-db/issues/5352)
- Buffered operations may share single RPC with read operation. Reducing the number of RPC calls speeds up `CREATE TABLE` statement. For example, the total number of RPC calls is dropped from 66 to 58 for a simple table like `CREATE TABLE t(k INT PRIMARY KEY)`. [#5177](https://github.com/yugabyte/yugabyte-db/issues/5177)
- Add new `ysqlsh` describe metacommands for tablegroups: `\dgr[+] [grpname]` to describe tablegroups, `\dgrt[+] [grpname]` lists all tables/indexes within the specified tablegroup (or within all tablegroups if `grpname` is not specified), and `\d <table_name>` is modified to include tablegroup information in the footer, if any. [#5088](https://github.com/yugabyte/yugabyte-db/issues/3118)
- Add `ALTER TABLEGROUP` statements to support `ALTER TABLEGROUP tablegroup_name RENAME TO ...` and `ALTER TABLEGROUP tablegroup_name OWNER TO ...`. Also changes `pg_tablegroup` entry corresponding to `tablegroup_name` to properly reflect new `grpname` and `grpowner` (if the user has proper permissions or ownership to issue the `ALTER TABLEGROUP` statement. [#5249](https://github.com/yugabyte/yugabyte-db/issues/5249)
- Add support for indexes to opt out of tablegroups (`NO TABLEGROUPS`) or select their own tablegroup (`TABLEGROUP group_name`). [#5293](https://github.com/yugabyte/yugabyte-db/issues/5293)
- For [`ysql_dump`](../../admin/ysql-dump), enable `serializable-deferrable` mode by default. Add new [`--no-serializable-deferrable`](../../../admin/ysql-dump/#no-serializable-deferrable) flag to disable the default mode. [#5128](https://github.com/yugabyte/yugabyte-db/issues/5128)
- Support `ALTER COLUMN type` for variants that don't require on-disk changes. Specifically only allowing `ALTER COLUMN type` for `varch(n)` and `varbit(n)`. For example, changing column type from `varchar(50)` to `varchar(255)`. [#4424](https://github.com/yugabyte/yugabyte-db/issues/4424)
- Enable `USING` clause in `DELETE` statement and `FROM` clause in `UPDATE` statement. [#738](https://github.com/yugabyte/yugabyte-db/issues/738) [#5262](https://github.com/yugabyte/yugabyte-db/issues/5262)
- Support GRANT/REVOKE/ALTER DEFAULT PRIVILEGES for tablegroups. [#5087](https://github.com/yugabyte/yugabyte-db/issues/5087) [#5160](https://github.com/yugabyte/yugabyte-db/issues/5160)
- Implement tablegroup query layer changes. [#4525](https://github.com/yugabyte/yugabyte-db/issues/4525)
- Replace "Foreign Scan" with "Seq Scan" for `EXPLAIN` statement. [#2076](https://github.com/yugabyte/yugabyte-db/issues/2076)
- Fix `initdb` when index backfill is enabled. [#5027](https://github.com/yugabyte/yugabyte-db/issues/5027)
- Properly handle empty delete with backfill by sending appropriate messages to the client, including setting it `skipped` and `rows_affected_count` to `0`. [#5015](https://github.com/yugabyte/yugabyte-db/issues/5015)
- `DROP INDEX` statement invalidates table cache entry for the index table, but should invalidate the table cache entry for the indexed table. [#4974](https://github.com/yugabyte/yugabyte-db/issues/4974)
- `yb-admin create_database_snapshot` command should not require `ysql.` prefix for database name. [#4991](https://github.com/yugabyte/yugabyte-db/issues/4991)
- For non-prepared statements, optimize `pg_statistic` system table lookups and update debugging utilities. [#5051](https://github.com/yugabyte/yugabyte-db/issues/5051)
- Correctly show beta feature warnings by default. [#5322](https://github.com/yugabyte/yugabyte-db/issues/5322)

##### YCQL

- For `WHERE` clause in the `CREATE INDEX` statement, return a `Not supported` error. [#5363](https://github.com/yugabyte/yugabyte-db/issues/5363)
- Fix TSAN issue in partition-aware policy for C++ driver 2.9.0-yb-8 (yugabyte/cassandra-cpp-driver). [#1837](https://github.com/yugabyte/yugabyte-db/issues/1837)
- Support YCQL backup for indexes based on JSON-attribute. [#5198](https://github.com/yugabyte/yugabyte-db/issues/5198)
- Correctly set `release_version` for `system.peers` queries. [#5407](https://github.com/yugabyte/yugabyte-db/issues/5407)
- Reject `TRUNCATE` operations when [`ycql_require_drop_privs_for_truncate`](../../../reference/configuration/yb-tserver/#ycql-require-drop-privs-for-truncate) flag is enabled. When enabled, `DROP TABLE` permission is required to truncate a table. Default is `false`. [#5443](https://github.com/yugabyte/yugabyte-db/issues/5443)
- Fix missing return statement in error case of the `SetPagingState` method in `statement_params.cc`. [#5441](https://github.com/yugabyte/yugabyte-db/issues/5441)
- Enable backfilling of transactional tables by default. [#4708](https://github.com/yugabyte/yugabyte-db/issues/4708)
- Fix `ycqlsh` should return a failure when known that the create (unique) index has failed. [#5161](https://github.com/yugabyte/yugabyte-db/issues/5161)

##### Core database

- Fix core dump related to DNS resolution from cache for Kubernetes universes. [#5561](https://github.com/yugabyte/yugabyte-db/issues/5561)
- Fix yb-master fails to restart after errors on first run. [#5276](https://github.com/yugabyte/yugabyte-db/issues/5276)
- Show better error message when using `yugabyted` and yb-master fails to start. [#5304](https://github.com/yugabyte/yugabyte-db/issues/5304)
- Disable ignoring deleted tablets on load by default. [#5122](https://github.com/yugabyte/yugabyte-db/issues/5122)
- [CDC] Improve CDC idle throttling logic to reduce high CPU utilization in clusters without workloads running. [#5472](https://github.com/yugabyte/yugabyte-db/issues/5472)
- For the `server_broadcast_addresses` flag, provide default port if not specified. [#2540](https://github.com/yugabyte/yugabyte-db/issues/2540)
- [CDC] Fix CDC TSAN destructor warning. [#4258](https://github.com/yugabyte/yugabyte-db/issues/4258)
- Add an API endpoint to download root certificate file. [#4957](https://github.com/yugabyte/yugabyte-db/issues/4957)
- Do not load deleted tables and tablets into memory on startup. [#5122](https://github.com/yugabyte/yugabyte-db/issues/5122)
- Set the follower lag for leaders to `0` by resetting timestamp to the maximum value when a peer becomes a leader and when the peer loses leadership. [#5502](https://github.com/yugabyte/yugabyte-db/issues/5502)
- Flow keyspace information from yb-master to yb-tserver. [#3020](https://github.com/yugabyte/yugabyte-db/issues/3020)
- Implement meta cache lookups throttling to reduce unnecessary thrashing. [#5434](https://github.com/yugabyte/yugabyte-db/issues/5434)
- Fix `rpcz/statements` links in `yb-tserver` Web UI when `pgsql_proxy_bind_address` and `cql_proxy_bind_address` are `0.0.0.0`. [#4963](https://github.com/yugabyte/yugabyte-db/issues/4963)
- `DumpReplayStateToStrings` should handle too many WAL entries scenario. Also, log lines only display fields critical for debugging and do not show customer-sensitive information. [#5345](https://github.com/yugabyte/yugabyte-db/issues/5345)
- Find the difference between the replica map and consensus state more quickly using a map lookup. [#5435](https://github.com/yugabyte/yugabyte-db/issues/5435)
- Improve failover to a new master leader in the case of a network partition or a dead tserver by registering tablet servers present in Raft quorums if they don't register themselves. Also, mark replicas that are in `BOOTSTRAPPING`/`NOT_STARTED` state with its true consensus information instead of marking it as a `NON_PARTICIPANT`. [#4691](https://github.com/yugabyte/yugabyte-db/issues/4691)
- Add YB-Master UI changes to support tablegroups and colocation. [#5086](https://github.com/yugabyte/yugabyte-db/issues/5086)
  - Adds a column to the user and index tables to display the YSQL OID information about the colocation parent table (if any).
  - For tablegroups, it displays the OID of the table's tablegroup (same as the OID present in `pg_tablegroup` as well as the `reltablegroup` column of `pg_class` for that relation).
  - For colocated databases, it displays the OID of the database.
  - Also, creates a table to display information about the parent tables. Wrap the names and uuid of the parent tables. Display YSQL OIDs as explained above.
- Fix `MetaCache::TAbleData::stale` is not getting reset back to `false`. [#5245](https://github.com/yugabyte/yugabyte-db/issues/5245)
- Do not crash yb-tserver when the `op_id` of the last WAL file is less than the committed `op_id`. [#1560](https://github.com/yugabyte/yugabyte-db/issues/1560)
- Add `CREATE TABLEGROUP` and `DROP TABLEGROUP` flow. [#4525](https://github.com/yugabyte/yugabyte-db/issues/4525)
- Add [`yb-admin list_snapshots`](../../../admin/yb-admin/#list_snapshots) `SHOW_DELETED` flag to show deleted snapshots that are still retained in memory. [#5332](https://github.com/yugabyte/yugabyte-db/issues/5332)
- Move read path functions to a separate file. [#4944](https://github.com/yugabyte/yugabyte-db/issues/4944)
- Wait for `yb_client_admin_operation_timeout_sec` for table creation in `yb-admin import_snapshot` task. [#5295](https://github.com/yugabyte/yugabyte-db/issues/5295)
- Create a cache for system-wide queries. [#5043](https://github.com/yugabyte/yugabyte-db/issues/5043)
- Annotate access to `enable_collect_cdc_metrics` flag and prevent TSAN warnings. [#5303](https://github.com/yugabyte/yugabyte-db/issues/5303)
- Raise yb-master and yb-tserver process ulimits to hard limits when appropriate. [#4943](https://github.com/yugabyte/yugabyte-db/issues/4943)
- Add a pre-flush step, triggered by yb-master, to snapshot operations to minimize the amount of data being flushed in sync. [#4718](https://github.com/yugabyte/yugabyte-db/issues/4718)
- Use Strand to remove intents and avoid wasting threads. [#5265](https://github.com/yugabyte/yugabyte-db/issues/5265)
- Add metric severity level [PORT] and sever-side level-based filtering for metrics and prometheus-metrics. [#4686](https://github.com/yugabyte/yugabyte-db/issues/5265)
- Decrease logging frequency for no local transaction status tablet from on every warning to once per second. [#5035](https://github.com/yugabyte/yugabyte-db/issues/5035)
- Fix MonoTime subtraction implementation to return `result` of the operation instead of `lhs`. [#5020](https://github.com/yugabyte/yugabyte-db/issues/5020)
- When dropping tables, delete snapshot directories for deleted tables. [#4756](https://github.com/yugabyte/yugabyte-db/issues/4756)
- Set up a global leader balance threshold while allowing progress across tables. Add [`load_balancer_max_concurrent_moves_per_table`](../../reference/configuration/yb-master/#load-balancer-max-concurrent-moves-per-table) and [`load_balancer_max_concurrent_moves`](../../reference/configuration/yb-master/#load-balancer-max-concurrent-moves) to improve performance of leader moves. And properly update state for leader stepdowns to prevent check failures. [#5021](https://github.com/yugabyte/yugabyte-db/issues/5021) [#5181](https://github.com/yugabyte/yugabyte-db/issues/5181)
- Reduce default value of [`load_balancer_max_concurrent_moves`](../../../reference/configuration/yb-master/#load-balancer-max-concurrent-moves) from `10` to `2`. [#5461](https://github.com/yugabyte/yugabyte-db/issues/5461)
- Improve bootstrap logic for resolving transaction statuses. [#5215](https://github.com/yugabyte/yugabyte-db/issues/5215)
- Avoid duplicate DNS lookup requests while handling `system.partitions` table requests. [#5225](https://github.com/yugabyte/yugabyte-db/issues/5225)
- Handle write operation failures during tablet bootstrap. [#5224](https://github.com/yugabyte/yugabyte-db/issues/5224)
- Ensure `strict_capacity_limit` is set when SetCapacity is called to prevent ASAN failures. [#5222](https://github.com/yugabyte/yugabyte-db/issues/5222)
- Drop index upon failed backfill. For YCQL, generate error to CREATE INDEX call if index backfill fails before the call is completed. [#5144](https://github.com/yugabyte/yugabyte-db/issues/5144) [#5161](https://github.com/yugabyte/yugabyte-db/issues/5161)
- Allow overflow of single-touch cache if multi-touch cache is not consuming space. [#4495](https://github.com/yugabyte/yugabyte-db/issues/4495)
- Don't flush RocksDB before restoration. [#5184](https://github.com/yugabyte/yugabyte-db/issues/5184)
- Fix `yugabyted` fails to start UI due to class binding failure. [#5069](https://github.com/yugabyte/yugabyte-db/issues/5069)
- On `yb-tserver` restart, prioritize bootstrapping transaction status tablets. [#4926](https://github.com/yugabyte/yugabyte-db/issues/4926)
- For `yb-admin` commands, clarify syntax for namespace. [#5482](https://github.com/yugabyte/yugabyte-db/issues/5482)
- Improve for-loops to avoid unnecessary copies and remove range-loop-analysis warnings. [#5458](https://github.com/yugabyte/yugabyte-db/issues/5458)
- Apply large transaction in a separate thread if the transaction will not fit into the limit of key-value pairs. The running transaction is removed from memory after all its intents are applied and removed. [#1923](https://github.com/yugabyte/yugabyte-db/issues/1923)
- Fix `SEGV` in Master UI when registering YB-TServer from Raft. [#5501](https://github.com/yugabyte/yugabyte-db/issues/5501)
- Heartbeats process only new consensus information unless `master_ignore_stale_cstate` flag is disabled. [#5301](https://github.com/yugabyte/yugabyte-db/issues/5301)
- Remove KernelStackWatchdog and use LongOperationTracker to waste less resources per thread. [#5226](https://github.com/yugabyte/yugabyte-db/issues/5226)
- Change the default value of `metric_node_name` flag from `DEFAULT_NODE_NAME` to `hostname:port`. [#4859](https://github.com/yugabyte/yugabyte-db/issues/4859)

##### Yugabyte Platform

- For S3 backups, install `s3cmd` required for encrypted backup and restore flows. [#5593](https://github.com/yugabyte/yugabyte-db/issues/5593)
- When creating on-premises provider, remove `YB_HOME_DIR` if not set. [#5592](https://github.com/yugabyte/yugabyte-db/issues/5592)
- Update to use templatized values for setting TLS flags in Helm Charts. [#5424](https://github.com/yugabyte/yugabyte-db/issues/5424)
- For YCQL client connectivity, allow downloading root certificate and key from **Certificates** menu. [#4957](https://github.com/yugabyte/yugabyte-db/issues/4957)
- Bypass CSRF checks when registering and logging in due to bug with CSRF token not being set. [#5533](https://github.com/yugabyte/yugabyte-db/issues/5533)
- Set node certificate field to subject of CA instead of issuer of CA. [#5377](https://github.com/yugabyte/yugabyte-db/issues/5377)
- Add option in universe creation form to control whether node_exporter is installed on provisioned nodes (including creating prometheus user + group). [#5421](https://github.com/yugabyte/yugabyte-db/issues/5421)
- Add Microsoft Azure provider UI and related changes to **Create Universe** form. [#5378](https://github.com/yugabyte/yugabyte-db/issues/5378)
- Create default network resources for multi-region deployments in Microsoft Azure if they don't exist and store for use during universe creation. [#5388](https://github.com/yugabyte/yugabyte-db/issues/5388)
- For Microsoft Azure, use preexisting network resources. [#5389](https://github.com/yugabyte/yugabyte-db/issues/5389)
- Set **Assign Public IP** correctly using the UI. [#5463](https://github.com/yugabyte/yugabyte-db/issues/5463)
- Fix `AreLeadersOnPreferredOnly` times out when setting preferred zones in **Edit Universe**. [#5406](https://github.com/yugabyte/yugabyte-db/issues/5406)
- Fix `yb_backup.py` script to remove `sudo` requirement and correctly change user. [#5440](https://github.com/yugabyte/yugabyte-db/issues/5440)
- Add CSRF tokens to forms. [#5419](https://github.com/yugabyte/yugabyte-db/issues/5419)
- Allow configuration of OIDC scope (for SSO use) as well as email attribute field. [#5465](https://github.com/yugabyte/yugabyte-db/issues/5465)
- If **Username** and **Password** are left empty in the **Custom SMTP Configuration**, remove `smtpUsername` and `smtpPassword` from the payload. [#5439](https://github.com/yugabyte/yugabyte-db/issues/5439)
- Add the **UTC** label to indicate `cron` expression must be relative to UTC. Also, add help text indicating when next scheduled job will run. [#4709](https://github.com/yugabyte/yugabyte-db/issues/4709)
- Move the source of truth for communication ports to the universe level. [#5353](https://github.com/yugabyte/yugabyte-db/issues/5353)
- Use appropriate range for time period (over entire step window rather than last minute) when querying Prometheus for metrics. [#4203](https://github.com/yugabyte/yugabyte-db/issues/4203)
- Support backing up encrypted at rest universes. [#3118](https://github.com/yugabyte/yugabyte-db/issues/3118)
- Only display "Rolling Upgrade Delay" if upgrade type is "Rolling". [#5362](https://github.com/yugabyte/yugabyte-db/issues/5362)
- Add guarding against never-ending loop when running the `yb_backup.py` script and yb-server doesn't have any data directories. [#5358](https://github.com/yugabyte/yugabyte-db/issues/5358)
- Fix metric panel displaying incorrect time interval when reloading after changing graph filter. [#5294](https://github.com/yugabyte/yugabyte-db/issues/5294)
- Fix logging on health check script failure. [#5340](https://github.com/yugabyte/yugabyte-db/issues/5294)
- Rename the label for enabling server-side encryption in S3 backups to **Encrypt Backup** for both backups and scheduled backup modals. [#5273](https://github.com/yugabyte/yugabyte-db/issues/5273)
- Add a VM backup script that includes Prometheus backups. [#5120](https://github.com/yugabyte/yugabyte-db/issues/5120)
- Add `BackupAdmin` role to authorize users to create backup and restore tasks on universes, but not given other administration privileges. [#4694](https://github.com/yugabyte/yugabyte-db/issues/4694)
- Modify the `yb_backup.py` script to add "round-robin" behavior to thread allocations to better spread loads across nodes in parallel during snapshot upload or download. [#4987](https://github.com/yugabyte/yugabyte-db/issues/4987)
- Fix the invalid `yb_backup.py` script behavior if `--keyspace` flag is not specified. [#5199](https://github.com/yugabyte/yugabyte-db/issues/5199)
- Add subtasks to check number of pods deployed during upgrades. [#5174](https://github.com/yugabyte/yugabyte-db/issues/5174)
- Make use of parallelism argument for `yb_backup.py` script. Explicitly pass the number of threads to concurrently run backup script through UI. The form defaults to the backup script default value of 8 threads if no value is provided. [#5283](https://github.com/yugabyte/yugabyte-db/issues/5283)
- The **Remove Node** action should be available if the **Add Node** action fails. [#5123](https://github.com/yugabyte/yugabyte-db/issues/5123)
- Add ability for user to specify the S3 backup target host that their bucket is located on. [#3684](https://github.com/yugabyte/yugabyte-db/issues/3684)
- Check if on-premise node instance has a universe that is using it and make a link to the Universe Overview page. [#4758](https://github.com/yugabyte/yugabyte-db/issues/4758)
- Fix `Cannot read property 'displayName' of undefined` error. [#5267](https://github.com/yugabyte/yugabyte-db/issues/5267)
- **Use Host Names** setting is not displaying correctly in **Provider Configuration** page. [#5252](https://github.com/yugabyte/yugabyte-db/issues/5252)
- Disable glob before running the cleanup of old log files. [#5169](https://github.com/yugabyte/yugabyte-db/issues/5169)
- Schedule job for `setUniverseKey` should handle errors on per-customer basis. [#3142](https://github.com/yugabyte/yugabyte-db/issues/3142)
- After Yugabyte Platform (Yugaware) restart, any in-progress tasks should be cancelled and marked as failed. [#5045](https://github.com/yugabyte/yugabyte-db/issues/5045)
- Redirect from login back to index if session is already validated. Logout should redirect to login endpoint. [#5019](https://github.com/yugabyte/yugabyte-db/issues/5019)
- Fix updating of **Universes** list after creating new universe. [#4784](https://github.com/yugabyte/yugabyte-db/issues/4784)
- Fix restore payload when renaming table to include keyspace. And check for keyspace if tableName is defined and return invalid request code when keyspace is missing. [#5178](https://github.com/yugabyte/yugabyte-db/issues/5178)
- Add the ability to override communication port default values during universe creation. [#5354](https://github.com/yugabyte/yugabyte-db/issues/5354)
