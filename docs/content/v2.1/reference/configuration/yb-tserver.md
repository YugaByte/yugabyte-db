---
title: yb-tserver configuration reference
headerTitle: yb-tserver
linkTitle: yb-tserver
description: YugabyteDB Tablet Server (yb-tserver) binary and configuration flags to store and manage data for client applications.
block_indexing: true
menu:
  v2.1:
    identifier: yb-tserver
    parent: configuration
    weight: 2440
isTocNested: true
showAsideToc: true
---

Use the `yb-tserver` binary and its flags to configure the [YB-TServer](../../../architecture/concepts/yb-tserver/) server. The `yb-tserver` executable file is located in the `bin` directory of YugabyteDB home.

## Syntax

```sh
yb-tserver [ flags ]
```

### Example

```sh
$ ./bin/yb-tserver \
--tserver_master_addrs 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
--rpc_bind_addresses 172.151.17.130 \
--start_pgsql_proxy \
--fs_data_dirs "/home/centos/disk1,/home/centos/disk2" &
```

### Online help

To display the online help, run `yb-tserver --help` from the YugabyteDB home directory.

```sh
$ ./bin/yb-tserver --help
```

## Configuration flags

- [Help](#help-flags)
- [General](#general-flags)
- [Logging](#logging-flags)
- [Raft](#raft-flags)
  - [Write Ahead Log (WAL)](#write-ahead-log-wal-flags)
- [Sharding](#sharding-flags)
- [Geo-distribution](#geo-distribution-flags)
- [YSQL](#ysql-flags)
- [YCQL](#ycql-flags)
- [YEDIS](#yedis-flags)
- [Performance](#performance-flags)
- [Security](#security-flags)
- [Change data capture (CDC)](#change-data-capture-cdc-flags)

---

### Help flags

##### --help

Displays help on all flags.

##### --helpon

Displays help on modules named by the specified flag value.

---

### General flags

##### --flagfile

Specifies the file to load the configuration flags from. The configuration flags must be in the same format as supported by the command line flags.

##### --version

Shows version and build info, then exits.

##### --tserver_master_addrs

Specifies a comma-separated list of all the `yb-master` RPC addresses.

Required.

Default: `127.0.0.1:7100`

{{< note title="Note" >}}

The number of comma-separated values should match the total number of YB-Master servers (or the replication factor).

{{< /note >}}

##### --fs_data_dirs

Specifies a comma-separated list of mount directories, where `yb-tserver` will add a `yb-data/tserver` data directory, `tserver.err`, `tserver.out`, and `pg_data` directory.

Required.

##### --fs_wal_dirs

Specifies a comma-separated list of directories, where `yb-tserver` will store write-ahead (WAL) logs. This can be the same as one of the directories listed in `--fs_data_dirs`, but not a subdirectory of a data directory.

Default: Same value as `--fs_data_dirs`

##### --max_clock_skew_usec

Specifies the expected maximum clock skew, in microseconds (µs), between any two nodes in your deployment.

Default: `50000` (50,000 µs = 50ms)

##### --rpc_bind_addresses

Specifies the comma-separated list of the network interface addresses to bind to for RPC connections.

- Typically, the value is set to the private IP address of the host on which the server is running. When using the default, or explicitly setting the value to `0.0.0.0:9100`, the server will listen on all available network interfaces.

- The values must match on all [`yb-master`](../yb-master/#rpc-bind-addresses) and `yb-tserver` configurations.

Default: `0.0.0.0:9100`

{{< note title="Note" >}}

In cases where `rpc_bind_addresses` is set to `0.0.0.0` (or not explicitly set, and uses the default) or in cases involving public IP addresses, make sure that [`server_broadcast_addresses`](#server-broadcast-addresses) is correctly set.

{{< /note >}}

##### --server_broadcast_addresses

Specifies the public IP or DNS hostname of the server (with an optional port). This value is used by servers to communicate with one another, depending on the connection policy parameter.

Default: `0.0.0.0:9100`

##### --use_private_ip

Specifies the policy that determines when to use private IP addresses for inter-node communication. Possible values are `never` (default),`zone`,`cloud` and `region`. Based on the values of the [placement (`--placement_*`) configuration flags](#placement-flags).

Valid values for the policy are:

- `never` — Always use the [`--server_broadcast_addresses`](#server-broadcast-addresses).
- `zone` — Use the private IP inside a zone; use the [`--server_broadcast_addresses`](#server-broadcast-addresses) outside the zone. 
- `region` — Use the private IP address across all zone in a region; use [`--server_broadcast_addresses`](#server-broadcast-addresses) outside the region.

Default: `never`

##### --webserver_interface

The address to bind for the web server user interface.

Default: `0.0.0.0` (`127.0.0.1`)

##### --webserver_port

The port for monitoring the web server.

Default: `9000`

##### --webserver_doc_root

The monitoring web server home directory..

Default: The `www` directory in the YugabyteDB home directory.

---

### Logging flags

##### --log_dir

The directory to write `yb-tserver` log files.

Default: Same as [`--fs_data_dirs`](#fs-data-dirs)

##### --logemaillevel

Email log messages logged at this level, or higher. Values: `0` (all), 1, 2, `3` (FATAL), `999` (none)

Default: `999`

##### --logmailer

The mailer used to send logging email messages.

Default: `"/bin/mail"`

##### --logtostderr

Write log messages to `stderr` instead of `logfiles`.

Default: `false`

##### --max_log_size

The maximum log size, in megabytes (MB). A value of `0` will be silently overridden to `1`.

Default: `1800` (1.8 GB)

##### --minloglevel

The minimum level to log messages. Values are: `0` (INFO), `1`, `2`, `3` (FATAL).

Default: `0` (INFO)

##### --stderrthreshold

Log messages at, or above, this level are copied to `stderr` in addition to log files.

Default: `2`

##### --stop_logging_if_full_disk

Stop attempting to log to disk if the disk is full.

Default: `false`

---

### Raft flags

{{< note title="Note" >}}

Ensure that values used for Raft and the write ahead log (WAL) in `yb-tserver` configurations match the values in `yb-master` configurations.

{{< /note >}}

##### --follower_unavailable_considered_failed_sec

The duration, in seconds, after which a follower is considered to be failed because the leader has not received a heartbeat. The follower is then evicted from the configuration and the data is rereplicated elsewhere.

Default: `900` (15 minutes)

{{< note title="Important" >}}

The `--follower_unavailable_considered_failed_sec` value should match the value for [`--log_min_seconds_to_retain`](#log-min-seconds-to-retain).

{{< /note >}}

##### --leader_failure_max_missed_heartbeat_periods

The maximum heartbeat periods that the leader can fail to heartbeat in before the leader is considered to be failed. The total failure timeout, in milliseconds (ms), is [`--raft_heartbeat_interval_ms`](#raft-heartbeat-interval-ms) multiplied by `--leader_failure_max_missed_heartbeat_periods`.

For read replica clusters, set the value to `10` in all `yb-tserver` and `yb-master` configurations.  Because the the data is globally replicated, RPC latencies are higher. Use this flag to increase the failure detection interval in such a higher RPC latency deployment.

Default: `6`

##### --raft_heartbeat_interval_ms

The heartbeat interval, in milliseconds (ms), for Raft replication. The leader produces heartbeats to followers at this interval. The followers expect a heartbeat at this interval and consider a leader to have failed if it misses several in a row.

Default: `500`

#### Write ahead log (WAL) flags

{{< note title="Note" >}}

Ensure that values used for the write ahead log (WAL) in `yb-tserver` configurations match the values for `yb-master` configurations.

{{< /note >}}

##### --fs_wal_dirs

The directory where the `yb-tserver` retains WAL files. May be the same as one of the directories listed in [`--fs_data_dirs`](#fs-data-dirs), but not a subdirectory of a data directory.

Default: Same as `--fs_data_dirs`

##### --durable_wal_write

If set to `false`, the writes to the WAL are synced to disk every [`interval_durable_wal_write_ms`](#interval-durable-wal-write-mas) milliseconds (ms) or every [`bytes_durable_wal_write_mb`](#bytes-durable-wal-write-mb) megabyte (MB), whichever comes first. This default setting is recommended only for multi-AZ or multi-region deployments where the availability zones (AZs) or regions are independent failure domains and there is not a risk of correlated power loss. For single AZ deployments, this flag should be set to `true`.

Default: `false`

##### --interval_durable_wal_write_ms

When [`--durable_wal_write`](#durable-wal-write) is false, writes to the WAL are synced to disk every `--interval_durable_wal_write_ms` or [`--bytes_durable_wal_write_mb`](#bytes-durable-wal-write-mb), whichever comes first.

Default: `1000`

##### --bytes_durable_wal_write_mb

When [`--durable_wal_write`](#durable-wal-write) is `false`, writes to the WAL are synced to disk every `--bytes_durable_wal_write_mb` or `--interval_durable_wal_write_ms`, whichever comes first.

Default: `1`

##### --log_min_seconds_to_retain

The minimum duration, in seconds, to retain WAL segments, regardless of durability requirements. WAL segments can be retained for a longer amount of time, if they are necessary for correct restart. This value should be set long enough such that a tablet server which has temporarily failed can be restarted within the given time period.

Default: `900` (15 minutes)

##### --log_min_segments_to_retain

The minimum number of WAL segments (files) to retain, regardless of durability requirements. The value must be at least `1`.

Default: `2`

##### --log_segment_size_mb

The size, in megabytes (MB), of a WAL segment (file). When the WAL segment reaches the specified size, then a log rollover occurs and a new WAL segment file is created.

Default: `64`

---

### Sharding flags

##### --yb_num_shards_per_tserver

The number of shards per YB-TServer for each YCQL table when a user table is created.

Default: `-1` (server internally sets default value). For servers with two or less CPU cores, then the default value is `4`. For four or more CPU cores, the default value is `8`. Local cluster installations, created with `yb-ctl` and `yb-docker-ctl`, use a value of `2` for this flag.

{{< note title="Important" >}}

This value must match on all `yb-master` and `yb-tserver` configurations of a YugabyteDB cluster.

{{< /note >}}

{{< note title="Note" >}}

On a per-table basis, the [`CREATE TABLE ... WITH TABLETS = <num>`](../../../api/ycql/ddl_create_table/#create-a-table-specifying-the-number-of-tablets) clause can be used to override the `yb_num_shards_per_tserver` value.

{{< /note >}}

##### --ysql_num_shards_per_tserver

The number of shards per YB-TServer for each YSQL table when a user table is created.

Default: `8`

{{< note title="Important" >}}

This value must match on all `yb-master` and `yb-tserver` configurations of a YugabyteDB cluster.

{{< /note >}}

{{< note title="Note" >}}

On a per-table basis, the [`CREATE TABLE ...SPLIT INTO`](../../../api/ysql/commands/ddl_create_table/#split-into) clause can be used to override the `ysql_num_shards_per_tserver` value.

{{< /note >}}

---

### Geo-distribution flags

Settings related to managing geo-distributed clusters.

##### --placement_zone

The name of the availability zone, or rack, where this instance is deployed.

Default: `rack1`

##### --placement_region

Specifies the name of the region, or data center, where this instance is deployed.

Default: `datacenter1`

##### --placement_cloud

Specifies the name of the cloud where this instance is deployed.

Default: `cloud1`

##### --placement_uuid

The unique identifier for the cluster.

Default: `""`

---

### YSQL flags

The following flags support the use of the [YSQL API](../../../api/ysql/).

##### --enable_ysql

Enables the YSQL API. Replaces the deprecated `--start_pgsql_proxy` flag.

Default: `true`

##### --ysql_enable_auth

Enables YSQL authentication.

{{< note title="Note" >}}

**Release 2.0:** Assign a password for the default `yugabyte` user to be able to sign in after enabling YSQL authentication.

**Release 2.0.1:** When YSQL authentication is enabled, you can sign into `ysqlsh` using the default `yugabyte` user that has a default password of `yugabyte`.

{{< /note >}}

Default: `false`

##### --pgsql_proxy_bind_address

Specifies the TCP/IP bind addresses for the YSQL API. The default value of `0.0.0.0:5433` allows listening for all IPv4 addresses access to localhost on port `5433`. The `--pgsql_proxy_bind_address` value overwrites `listen_addresses` (default value of `127.0.0.1:5433`) that controls which interfaces accept connection attempts.

To specify fine-grained access control over who can access the server, use [`--ysql_hba_conf`](#ysql-hba-conf).

Default: `0.0.0.0:5433`

##### --pgsql_proxy_webserver_port

Specifies the web server port for YSQL metrics monitoring.

Default: `13000`

##### --ysql_hba_conf

Specifies a comma-separated list of PostgreSQL client authentication settings that is written to the `ysql_hba.conf` file.

For details on using `--ysql_hba_conf` to specify client authentication, see [Fine-grained authentication](../../../secure/authentication/client-authentication).

Default: `"host all all 0.0.0.0/0 trust,host all all ::0/0 trust"`

##### --ysql_pg_conf

Comma-separated list of PostgreSQL setting assignments.

##### --ysql_timezone

Specifies the time zone for displaying and interpreting timestamps.

Default: Uses the YSQL time zone.

##### --ysql_datestyle

Specifies the display format for data and time values.

Default: Uses the YSQL display format.

##### --ysql_max_connections

Specifies the maximum number of concurrent YSQL connections.

Default: `300`

##### --ysql_default_transaction_isolation

Specifies the default transaction isolation level.

Valid values: `SERIALIZABLE`, `REPEATABLE READ`, `READ COMMITTED`, and `READ UNCOMMITTED`.

Default: `REPEATABLE READ`

{{< note title="Note" >}}

YugabyteDB supports only two transaction isolation levels: `REPEATABLE READ` (aka snapshot) and `SERIALIZABLE`. The transaction isolation levels of `READ UNCOMMITTED` and `READ COMMITTED` are implemented in YugabyteDB as `REPEATABLE READ`.

{{< /note >}}

##### --ysql_log_statement

Specifies the types of YSQL statements that should be logged. 

Valid values: `none` (off), `ddl` (only data definition queries, such as create/alter/drop), `mod` (all modifying/write statements, includes DDLs plus insert/update/delete/trunctate, etc), and `all` (all statements).

Default: `none`

##### --ysql_log_min_messages

Specifies the lowest YSQL message level to log.

---

### YCQL flags

The following flags support the use of the [YCQL API](../../../api/ycql/).

##### --use_cassandra_authentication

Specify `true` to enable YCQL authentication (`username` and `password`), enable YCQL security statements (`CREATE ROLE`, `DROP ROLE`, `GRANT ROLE`, `REVOKE ROLE`, `GRANT PERMISSION`, and `REVOKE PERMISSION`), and enforce permissions for YCQL statements.

Default: `false`

##### --cql_proxy_bind_address

Specifies the bind address for the YCQL API.

Default: `0.0.0.0:9042` (`127.0.0.1:9042`)

##### --cql_proxy_webserver_port

Specifies the port for monitoring YCQL metrics.

Default: `12000`


##### --cql_table_is_transactional_by_default

Specifies if YCQL tables are created with transactions enabled by default.

Default: `false`


---

### YEDIS flags

The following flags support the use of the YEDIS API.

##### --redis_proxy_bind_address

Specifies the bind address for the YEDIS API.

Default: `0.0.0.0:6379`

##### --redis_proxy_webserver_port

Specifies the port for monitoring YEDIS metrics.

Default: `11000`

---

### Performance flags

##### --enable_ondisk_compression

Enable Snappy compression at the the cluster level.

Default: `true`

##### --rocksdb_compact_flush_rate_limit_bytes_per_sec

Used to control rate of memstore flush and SSTable file compaction.

Default: `256MB`

##### --rocksdb_universal_compaction_min_merge_width

Compactions run only if there are at least `rocksdb_universal_compaction_min_merge_width` eligible files and 
their running total (summation of size of files considered so far) is 
within `rocksdb_universal_compaction_size_ratio` of the next file in consideration to be included into the same compaction.

Default: `4`

##### --rocksdb_universal_compaction_size_ratio

Compactions run only if there are at least `rocksdb_universal_compaction_min_merge_width` eligible files and 
their running total (summation of size of files considered so far) is 
within `rocksdb_universal_compaction_size_ratio` of the next file in consideration to be included into the same compaction.

Default: `20`

##### --timestamp_history_retention_interval_sec

The time interval, in seconds, to retain history/older versions of data. Point-in-time reads at a hybrid time prior to this interval 
might not be allowed after a compaction and return a `Snapshot too old` error. 
Set this to be greater than the expected maximum duration of any single transaction in your application.

Default: `120`

##### --remote_bootstrap_rate_limit_bytes_per_sec

Rate control across all tablets being remote bootstrapped from or to this process.

Default: `256MB`

---

### Security flags

For details on enabling client-server encryption, see [Client-server encryption](../../../secure/tls-encryption/client-to-server).

##### --certs_dir

Directory that contains certificate authority, private key, and certificates for this server.

Default: `""` (Uses `<data drive>/yb-data/tserver/data/certs`.)

##### --allow_insecure_connections

Allow insecure connections. Set to `false` to prevent any process with unencrypted communication from joining a cluster. Note that this flag requires the [`use_node_to_node_encryption`](#use-node-to-node-encryption) to be enabled and [`use_client_to_server_encryption`](#use-client-to-server-encryption) to be enabled.

Default: `true`

##### --certs_for_client_dir

The directory that contains certificate authority, private key, and certificates for this server that should be used for client-to-server communications.

Default: `""` (Use the same directory as for server-to-server communications.)

##### --dump_certificate_entries

Dump certificate entries.

Default: `false`

##### --use_client_to_server_encryption

Use client-to-server, or client-server, encryption with YCQL. 

Default: `false`

##### --use_node_to_node_encryption

Enable server-server, or node-to-node, encryption between YugabyteDB YB-Master and YB-TServer servers in a cluster or universe. To work properly, all YB-Master servers must also have their [`--use_node_to_node_encryption`](../yb-master/#use-node-to-node-encryption) setting enabled. When enabled, then [`--allow_insecure_connections`](#allow-insecure-connections) must be disabled.

Default: `false`

---

### Change data capture (CDC) flags

To learn about CDC, see [Change data capture (CDC)](../../../architecture/cdc-architecture).

##### --cdc_rpc_timeout_ms

The timeout used for CDC->`yb-tserver` asynchronous RPC calls.

Default: `30000`

##### --cdc_state_checkpoint_update_interval_ms

The rate at which CDC state's checkpoint is updated.

Default: `15000`

##### --cdc_ybclient_reactor_threads

The number of reactor threads to be used for processing `ybclient` requests for CDC.

Default: `50`

## Admin UI

The Admin UI for the YB-TServer is available at `http://localhost:9000`.

### Home

Home page of the YB-TServer (`yb-tserver`) that gives a high level overview of this specific instance.

![tserver-home](/images/admin/tserver-home.png)

### Dashboards

Here's a list of all dashboards to review the ongoing operations:

![tserver-dashboards](/images/admin/tserver-dashboards.png)

### Tablets

List of all tablets managed by this specific instance, sorted by the table name.

![tserver-tablets](/images/admin/tserver-tablets.png)

### Debug

List of all utilities available to debug the performance of this specific instance.

![tserver-debug](/images/admin/tserver-debug.png)
