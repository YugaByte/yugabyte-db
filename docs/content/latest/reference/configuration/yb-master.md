---
title: yb-master configuration reference
headerTitle: yb-master
linkTitle: yb-master
description: YugabyteDB Master Server (yb-master) binary and configuration flags to manage cluster metadata and coordinate cluster-wide operations.
menu:
  latest:
    identifier: yb-master
    parent: configuration
    weight: 2450
aliases:
  - /latest/admin/yb-master
isTocNested: true
showAsideToc: true
---

Use the `yb-master` binary and its flags to configure the [YB-Master](../../../architecture/concepts/yb-master) server. The `yb-master` executable file is located in the `bin` directory of YugabyteDB home.

## Syntax

```sh
yb-master [ flag  ] | [ flag ]
```

### Example 

```sh
$ ./bin/yb-master \
--master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
--rpc_bind_addresses 172.151.17.130 \
--fs_data_dirs "/home/centos/disk1,/home/centos/disk2" \
--replication_factor=3
```

### Online help

To display the online help, run `yb-master --help` from the YugabyteDB home directory.

```sh
$ ./bin/yb-master --help
```

## Configuration flags

- [General](#general-flags)
- [YSQL](#ysql-flags)
- [Logging](#logging-flags)
- [Raft](#raft-flags)
  - [Write ahead log (WAL)](#write-ahead-log-wal-flags)
- [Sharding](#sharding-flags)
- [Load balancing](#load-balancing-flags)
- [Geo-distribution](#geo-distribution-flags)
- [Security](#security-flags)
- [Change data capture (CDC)](#change-data-capture-cdc-flags)

---

### General flags

##### --version

Shows version and build information, then exits.

##### --flagfile

Specifies the configuration file to load flags from.

##### --master_addresses

Specifies a comma-separated list of all RPC addresses for `yb-master` consensus-configuration.

{{< note title="Note" >}}

The number of comma-separated values should match the total number of YB-Master server (or the replication factor).

{{< /note >}}

Required.

Default: `127.0.0.1:7100`

##### --fs_data_dirs

Specifies a comma-separated list of mount directories, where `yb-master` will add a `yb-data/master` data directory, `master.err`, `master.out`, and `pg_data` directory.

Required.

##### --fs_wal_dirs

Specifies a comma-separated list of directories, where `yb-master` will store write-ahead (WAL) logs. This can be the same as one of the directories listed in `--fs_data_dirs`, but not a subdirectory of a data directory.

Default: Same value as `--fs_data_dirs`

##### --rpc_bind_addresses

Specifies the comma-separated list of the network interface addresses to bind to for RPC connections

- Typically, the value is set to the private IP address of the host on which the server is running. When using the default, or explicitly setting the value to `0.0.0.0:7100`, the server will listen on all available network interfaces.

- The values used must match on all `yb-master` and [`yb-tserver`](../yb-tserver/#rpc-bind-addresses) configurations.

Default: `0.0.0.0:7100`

{{< note title="Note" >}}

In cases where `rpc_bind_addresses` is set to `0.0.0.0` (or not explicitly set, and uses the default) or in cases involving public IP addresses, make sure that [`server_broadcast_addresses`](#server-broadcast-addresses) is correctly set.

{{< /note >}}

##### --server_broadcast_addresses

Specifies the public IP or DNS hostname of the server (with an optional port). This value is used by servers to communicate with one another, depending on the connection policy parameter.

Default: `""`

##### --dns_cache_expiration_ms

Specifies the duration, in milliseconds, until a cached DNS resolution expires. When hostnames are used instead of IP addresses, a DNS resolver must be queried to match hostnames to IP addresses. By using a local DNS cache to temporarily store DNS lookups, DNS queries can be resolved quicker and additional queries can be avoided, thereby reducing latency, improving load times, and reducing bandwidth and CPU consumption.

Default: `60000` (1 minute)

{{< note title="Note" >}}

If this value is changed from the default, make sure to add the same value to all YB-Master and YB-TSever configurations.

{{< /note >}}

##### --use_private_ip

Specifies the policy that determines when to use private IP addresses for inter-node communication. Possible values are `never` (default),`zone`,`cloud` and `region`. Based on the values of the [placement (`--placement_*`) configuration flags](#placement-flags).

Valid values for the policy are:

- `never` — Always use the [`--server_broadcast_addresses`](#server-broadcast-addresses).
- `zone` — Use the private IP inside a zone; use the [`--server_broadcast_addresses`](#server-broadcast-addresses) outside the zone. 
- `region` — Use the private IP address across all zone in a region; use [`--server_broadcast_addresses`](#server-broadcast-addresses) outside the region.

Default: `never`

##### --webserver_interface

Specifies the bind address for web server user interface access.

Default: `0.0.0.0`

##### --webserver_port

Specifies the web server monitoring port.

Default: `7000`

##### --webserver_doc_root

Monitoring web server home.

Default: The `www` directory in the YugabyteDB home directory.

---

### YSQL flags

##### --enable_ysql

Enables the YSQL API when value is `true`. Replaces the deprecated `--start_pgsql_proxy` flag.

Default: `true`

---

### Logging flags

##### --alsologtoemail

Sends log messages to these email addresses in addition to logfiles.

Default: `""`

##### --colorlogtostderr

Color messages logged to `stderr` (if supported by terminal).

Default: `false`

##### --logbuflevel

Buffer log messages logged at this level (or lower).

Valid values: `-1` (don't buffer); `0` (INFO); `1` (WARN); `2` (ERROR); `3` (FATAL)

Default: `0`

##### --logbufsecs

Buffer log messages for at most this many seconds.

Default: `30`

##### --logemaillevel

Email log messages logged at this level, or higher. 

Values: `0` (all); `1` (WARN), `2` (ERROR), `3` (FATAL), `999` (none)

Default: `999`

##### --logmailer

The mailer used to send logging email messages.

Default: `"/bin/mail"

##### --logtostderr

Write log messages to `stderr` instead of `logfiles`.

Default: `false`

##### --log_dir

The directory to write `yb-master` log files.

Default: Same as [`--fs_data_dirs`](#fs-data-dirs)

##### --log_link

Put additional links to the log files in this directory.

Default: `""`

##### --log_prefix

Prepend the log prefix to each log line.

Default:  `true`

##### --max_log_size

The maximum log size, in megabytes (MB). A value of `0` will be silently overridden to `1`.

Default: `1800` (1.8 GB)

##### --minloglevel

The minimum level to log messages. Values are: `0` (INFO), `1` (WARN), `2` (ERROR), `3` (FATAL).

Default: `0` (INFO)

##### --stderrthreshold

Log messages at, or above, this level are copied to `stderr` in addition to log files.

Default: `2`

---

### Raft flags

{{< note title="Note" >}}

Ensure that values used for Raft and the write ahead log (WAL) in `yb-master` configurations match the values in `yb-tserver` configurations.

{{< /note >}}

##### --follower_unavailable_considered_failed_sec

The duration, in seconds, after which a follower is considered to be failed because the leader has not received a heartbeat. The follower is then evicted from the configuration and the data is rereplicated elsewhere.

Default: `900` (15 minutes)

{{< note title="Important" >}}

The `--follower_unavailable_considered_failed_sec` value should match the value for [`--log_min_seconds_to_retain`](#log-min-seconds-to-retain).

{{< /note >}}

##### --leader_failure_max_missed_heartbeat_periods

The maximum heartbeat periods that the leader can fail to heartbeat in before the leader is considered to be failed. The total failure timeout, in milliseconds, is [`--raft_heartbeat_interval_ms`](#raft-heartbeat-interval-ms) multiplied by `--leader_failure_max_missed_heartbeat_periods`.

For read replica clusters, set the value to `10` in all `yb-tserver` and `yb-master` configurations.  Because the the data is globally replicated, RPC latencies are higher. Use this flag to increase the failure detection interval in such a higher RPC latency deployment.

Default: `6`

##### --raft_heartbeat_interval_ms

The heartbeat interval, in milliseconds, for Raft replication. The leader produces heartbeats to followers at this interval. The followers expect a heartbeat at this interval and consider a leader to have failed if it misses several in a row.

Default: `500`

#### Write ahead log (WAL) flags

{{< note title="Note" >}}

Ensure that values used for the write ahead log (WAL) in `yb-master` configurations match the values in `yb-tserver` configurations.

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

### Load balancing flags

For information on YB-Master load balancing, see [Data placement and load balancing](../../../architecture/concepts/yb-master/#data-placement-and-load-balancing)

For load balancing commands in `yb-admin`, see [Rebalancing commands (yb-admin)](../../../admin/yb-admin/#rebalancing-commands).

##### --enable_load_balancing

Enables or disables the load balancing algorithm, to move tablets around.

Default: `true`

##### --leader_balance_threshold

Specifies the number of leaders per tablet server to balance below. If this is configured to `0` (the default), the leaders will be balanced optimally at extra cost.

Default: `0`

##### --leader_balance_unresponsive_timeout_ms

Specifies the period of time, in milliseconds, that a YB-Master can go without receiving a heartbeat from a YB-TServer before considering it unresponsive. Unresponsive servers are excluded from leader balancing.

Default: `3000` (3 seconds)

##### --load_balancer_max_concurrent_adds

Specifies the maximum number of tablet peer replicas to add in a load balancer operations.

Default: `1`

##### --load_balancer_max_concurrent_moves

Specifies the maximum number of tablet leaders on tablet servers (across the cluster) to move in a load balancer operation.

Default: `2`

##### --load_balancer_max_concurrent_moves_per_table

Specifies the maximum number of tablet leaders per table to move in any one run of the load balancer. The maximum number of tablet leader moves across the cluster is still limited by the flag `load_balancer_max_concurrent_moves`. This flag is meant to prevent a single table from using all of the leader moves quota and starving other tables.

Default: `1`

##### --load_balancer_max_concurrent_removals

Specifies the maximum number of over-replicated tablet peer removals to do in a load balancer operation.

Default: `1`

##### --load_balancer_max_concurrent_tablet_remote_bootstraps

Specifies the maximum number of tablets being remote bootstrapped across the cluster.

Default: `10`

##### --load_balancer_max_concurrent_tablet_remote_bootstraps_per_table

 Maximum number of tablets being remote bootstrapped for any table. The maximum number of remote bootstraps across the cluster is still limited by the flag `load_balancer_max_concurrent_tablet_remote_bootstraps`. This flag is meant to prevent a single table use all the available remote bootstrap sessions and starving other tables.

Default: `2`

##### --load_balancer_max_over_replicated_tablets

Specifies the maximum number of running tablet replicas that are allowed to be over the configured replication factor.

Default: `1`

##### --load_balancer_num_idle_runs

Specifies the number of idle runs of load balancer to deem it idle.

Default: `5`

##### --load_balancer_skip_leader_as_remove_victim

Should the LB skip a leader as a possible remove candidate. 

Default: `false`

---

### Sharding flags

##### --max_clock_skew_usec

The expected maximum clock skew, in microseconds (µs), between any two servers in your deployment.

Default: `500000` (500,000 µs = 500ms)

##### --replication_factor

The number of replicas, or copies of data, to store for each tablet in the universe.

Default: `3`

##### --yb_num_shards_per_tserver

The number of shards per YB-TServer for each YCQL table when a user table is created.

Default: `-1` (server internally sets default value). For servers with two or less CPU cores, then the default value is `4`. For four or more CPU cores, the default value is `8`. Local cluster installations, created using `yb-ctl` and `yb-docker-ctl`, use a value of `2` for this flag.

{{< note title="Important" >}}

This value must match on all `yb-master` and `yb-tserver` configurations of a YugabyteDB cluster.

{{< /note >}}

{{< note title="Note" >}}

On a per-table basis, the [`CREATE TABLE ... WITH TABLETS = <num>`](../../../api/ycql/ddl_create_table/#create-a-table-specifying-the-number-of-tablets) clause can be used to override the `yb_num_shards_per_tserver` value.

{{< /note >}}

##### --ysql_num_shards_per_tserver

The number of shards per YB-TServer for each YSQL table when a user table is created.

Default: `8`

{{< note title="Note" >}}

On a per-table basis, the [`CREATE TABLE ...SPLIT INTO`](../../../api/ysql/the-sql-language/statements/ddl_create_table/#split-into) clause can be used to override the `ysql_num_shards_per_tserver` value.

{{< /note >}}

#### --tablet_split_size_threshold_bytes

{{< note title="Note" >}}

Automatic tablet splitting is currently in [BETA](../../../faq/general/#what-is-the-definition-of-the-beta-feature-tag).

{{< /note >}}

Enables tablets to automatically split tablets while online, based on the specified tablet threshold size.

**Syntax**

```sh
yb-admin --master_addresses <master-addresses> --tablet_split_size_threshold_bytes <bytes>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *bytes*: The threshold size, in bytes, after which tablets should be split. Default value of `0` disables automatic tablet splitting.

For details on automatic tablet splitting, see:

- [Automatic tablet splitting](../../../architecture/docdb-sharding/tablet-splitting) — Architecture overview
- [Automatic Re-sharding of Data with Tablet Splitting](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-automatic-tablet-splitting.md) — Architecture design document in the GitHub repository.

---

### Geo-distribution flags

Settings related to managing geo-distributed clusters.

##### --placement_zone

The name of the availability zone (AZ), or rack, where this instance is deployed.

Default: `rack1`

##### --placement_region

Name of the region or data center where this instance is deployed.

Default: `datacenter1`

##### --placement_cloud

Name of the cloud where this instance is deployed.

Default: `cloud1`

##### --placement_uuid

The unique identifier for the cluster.

Default: `""`

##### -- use_private_ip

Determines when to use private IP addresses. Possible values are `never` (default),`zone`,`cloud` and `region`. Based on the values of the `placement_*` configuration flags.

Default: `never`

---

### Security flags

For details on enabling server-to-server encryption, see [Server-server encryption](../../../secure/tls-encryption/server-to-server).

##### --certs_dir

Directory that contains certificate authority, private key, and certificates for this server.

Default: `""` (Uses `<data drive>/yb-data/master/data/certs`.)

##### --allow_insecure_connections

Allow insecure connections. Set to `false` to prevent any process with unencrypted communication from joining a cluster. Note that this flag requires the [`use_node_to_node_encryption`](#use-node-to-node-encryption) to be enabled.

Default: `true`

##### --dump_certificate_entries

Adds certificate entries, including IP addresses and hostnames, to log for handshake error messages.  Enabling this flag is useful for debugging certificate issues.

Default: `false`

##### --use_node_to_node_encryption

Enable server-server, or node-to-node, encryption between YugabyteDB YB-Master and YB-TServer servers in a cluster or universe. To work properly, all YB-Master servers must also have their [`--use_node_to_node_encryption`](../yb-master/#use-node-to-node-encryption) flag enabled. When enabled, then [`--allow_insecure_connections`](#allow-insecure-connections) flag must be disabled.

Default: `false`

---

### Change data capture (CDC) flags

To learn more about CDC, see [Change data capture (CDC)](../../../architecture/cdc-architecture).

For other CDC configuration flags, see [YB-TServer's CDC flags](../yb-tserver/#change-data-capture-cdc-flags).

##### --cdc_state_table_num_tablets

The number of tablets to use when creating the CDC state table.

Default: `0` (Use the same default number of tablets as for regular tables.)

##### --cdc_wal_retention_time_secs

WAL retention time, in seconds, to be used for tables for which a CDC stream was created. If you change the value, make sure that the [`yb-tserver --cdc_wal_retention_time_secs`](../yb-tserver/#cdc-wal-retention-time-secs) flag is also updated with the same value.

Default: `14400` (4 hours)

## Admin UI

The Admin UI for yb-master is available at http://localhost:7000.

### Home

Home page of the YB-Master server that gives a high level overview of the cluster. Note all YB-Master servers in a cluster show identical information.

![master-home](/images/admin/master-home-binary-with-tables.png)

### Tables

List of tables present in the cluster.

![master-tables](/images/admin/master-tables.png)

### Tablet servers

List of all nodes (aka YB-TServer servers) present in the cluster.

![master-tservers](/images/admin/master-tservers-list-binary-with-tablets.png)

### Debug

List of all utilities available to debug the performance of the cluster.

![master-debug](/images/admin/master-debug.png)
