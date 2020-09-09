---
title: yb-admin - command line tool for advanced YugabyteDB administration
headerTitle: yb-admin
linkTitle: yb-admin
description: Use the yb-admin command line tool for advanced administration of YugabyteDB clusters.
block_indexing: true
menu:
  v2.1:
    identifier: yb-admin
    parent: admin
    weight: 2465
isTocNested: true
showAsideToc: true
---

The `yb-admin` utility, located in the `bin` directory of YugabyteDB home, provides a command line interface for administering clusters.

It invokes the [`yb-master`](../../reference/configuration/yb-master/) and [`yb-tserver`](../../reference/configuration/yb-tserver/) servers to perform the necessary administration.

## Syntax

To use the `yb-admin` utility from the YugabyteDB home directory, run `./bin/yb-admin` using the following syntax.

```sh
yb-admin [ -master_addresses <master-addresses> ]  [ -timeout_ms <millisec> ] [ -certs_dir_name <dir_name> ] <command> [ command_flags ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- timeout_ms: The RPC timeout, in milliseconds. Default value is `60000`. A value of `0` means don't wait; `-1` means wait indefinitely.
- certs_dir_name: The directory with certificates to use for secure server connections. Default value is `""`.
  - To connect to a cluster with TLS enabled, you must include the `-certs_dir_name` flag with the directory location where the root certificate is located.
- *command*: The operation to be performed. See command for syntax details and examples.
- *command_flags*: Configuration flags that can be applied to the command.

### Online help

To display the online help, run `yb-admin --help` from the YugabyteDB home directory.

```sh
$ ./bin/yb-admin --help
```

## Commands

- [Universe and cluster](#universe-and-cluster-commands)
- [Table](#table-commands)
- [Backup and snapshot](#backup-and-snapshot-commands)
- [Deployment topology](#deployment-topology-commands)
  - [Multi-zone and multi-region](#multi-zone-and-multi-region-commands)
  - [Master-follower](#master-follower-commands)
  - [Read replica](#read-replica-commands)
- [Change data capture (CDC)](#change-data-capture-cdc-commands)
- [Decommissioning](#decommissioning-commands)
- [Rebalancing](#rebalancing-commands)
- [Security](#security-commands)
  - [Encryption at rest](#encryption-at-rest-commands)

---

### Universe and cluster commands

#### get_universe_config

Gets the configuration for the universe.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> get_universe_config
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

#### change_config

Changes the configuration of a tablet.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> change_config <tablet_id> [ ADD_SERVER | REMOVE_SERVER ] <peer_uuid> [ PRE_VOTER | PRE_OBSERVER ]
```

- master_addresses: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *tablet_id*: The identifier (ID) of the tablet.
- ADD SERVER | REMOVE SERVER: Subcommand to add or remove the server.
- *peer_uuid*: The UUID of the peer.
- PRE_VOTER | PRE_OBSERVER: Role of the new peer joining the quorum. Required when using the `ADD_SERVER` subcommand.

**Notes:**

If you need to take a node down temporarily, but intend to bring it back up, you should not need to use the `REMOVE_SERVER` subcommand.

- If the node is down for less than 15 minutes, it will catch up through RPC calls when it comes back online.
- If the node is offline longer than 15 minutes, then it will go through Remote Bootstrap, where the current leader will forward all relevant files to catch up.

If you do not intend to bring a node back up (perhaps you brought it down for maintenance, but discovered that the disk is bad), then you want to decommission the node (using the `REMOTE_SERVER` subcommand) and then add in a new node (using the `ADD_SERVER` subcommand).

#### change_master_config

Changes the master configuration.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> change_master_config [ ADD_SERVER|REMOVE_SERVER ] <ip_addr> <port> [ 0 | 1 ]
```

- master_addresses: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- ADD_SERVER | REMOVE_SERVER: Adds or removes a new YB-Master server.
  - After adding or removing a node, verify the status of the YB-Master server on the YB-Master UI page (http://node-ip:7000) or run the [`yb-admin dump_masters_state` command](#dump-masters-state).
- *ip_addr*: The IP address of the server node.
- *port*: The port of the server node.
- `0` | `1`: Disabled (`0`) or enabled (`1`). Default is `1`.

#### list_tablet_servers

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_tablet_servers <tablet_id>
```

- master_addresses: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *tablet_id*: The identifier (ID) of the tablet.

#### list_tablets

Lists all tablets and their replica locations for a particular table.

Useful to find out who the LEADER of a tablet is.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_tablets <keyspace> <table_name> [max_tablets]
```

- master_addresses: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *keyspace*: The namespace, or name of the database or keyspace.
- *table_name*: The name of the table.
- *max_tablets*: The maximum number of tables to be returned. Default is `10`. Set to `0` to return all tablets.

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 list_tablets ydb test_tb 0
```

```
Tablet UUID                       Range                                                     Leader
cea3aaac2f10460a880b0b4a2a4b652a  partition_key_start: "" partition_key_end: "\177\377"     127.0.0.1:9100
e509cf8eedba410ba3b60c7e9138d479  partition_key_start: "\177\377" partition_key_end: ""
```

#### list_all_tablet_servers

Lists all tablet servers.

**Syntax**

```
yb-admin -master_addresses <master-addresses> list_all_tablet_servers
```

- master-addresses: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

#### list_all_masters

Displays a list of all YB-Master servers in a table listing the master UUID, RPC host and port, state (`ALIVE` or `DEAD`), and role (`LEADER`, `FOLLOWER`, or `UNKNOWN_ROLE`).

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_all_masters
```

- master-addresses: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

**Example**

```sh
$ ./bin/yb-admin -master_addresses node7:7100,node8:7100,node9:7100 list_all_masters
```

```
Master UUID         RPC Host/Port          State      Role
...                   node8:7100           ALIVE     FOLLOWER
...                   node9:7100           ALIVE     FOLLOWER
...                   node7:7100           ALIVE     LEADER
```

#### list_replica_type_counts

Prints a list of replica types and counts for the specified table.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_replica_type_counts <keyspace> <table_name>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *keyspace*: The name of the database or keyspace.
- *table_name*: The name of the table.

#### dump_masters_state

Prints the status of the YB-Master servers.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> dump_masters_state
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

#### list_tablet_server_log_locations

List the locations of the tablet server logs.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_tablet_server_log_locations
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

#### list_tablets_for_tablet_server

Lists all tablets for the specified tablet server (YB-TServer).

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_tablets_for_tablet_server <ts_uuid>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *ts_uuid*: The UUID of the tablet server (YB-TServer).

#### master_leader_stepdown

Forces the master leader to step down. The specified YB-Master node will take its place as leader.

{{< note title="Note" >}}

- Use this command only if recommended by Yugabyte support.

- There is a possibility of downtime.

{{< /note >}}

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> master_leader_stepdown <new_leader_id>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *new_leader_id*: The identifier (ID) of the new YB-Master leader.

---

### Table commands

#### list_tables

Prints a list of all tables.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_tables
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

Returns tables in the following format:

```
<namespace>.<table_name>
```

- *namespace*: Name of the database or keyspace.
- *table_name*: Name of the table.

{{< note title="Tip" >}}

To display a list of tables and their UUID (`table_id`) values, open the **YB-Master UI** (`<master_host>:7000/`) and click **Tables** in the navigation bar.

{{< /note >}}

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 list_tables
```

```
...
yugabyte.pg_range
template1.pg_attrdef
template0.pg_attrdef_adrelid_adnum_index
template1.pg_conversion
system_platform.pg_opfamily
postgres.pg_opfamily_am_name_nsp_index
system_schema.functions
template0.pg_statistic
system.local
template1.pg_inherits_parent_index
template1.pg_amproc
system_platform.pg_rewrite
yugabyte.pg_ts_config_cfgname_index
template1.pg_trigger_tgconstraint_index
template1.pg_class
template1.pg_largeobject
system_platform.sql_parts
template1.pg_inherits
...
```

#### list_tables_with_db_types

Prints a list of all tables, prefixed by the database type (`ysql`, `ycql`, or `yedis`)

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_tables_with_db_types
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

Returns tables in the following format:

```
<db_type>.<namespace>.<table_name>
```

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 list_tables_with_db_types
```

```
ycql.system_schema.indexes
ycql.system_schema.keyspaces
ycql.system_schema.sys.catalog
ycql.system_schema.tables
ycql.system_schema.triggers
ycql.system_schema.types
ycql.system_schema.views
ysql.postgres.sql_features
ysql.postgres.sql_implementation_info
ysql.postgres.sql_languages
ysql.postgres.sql_packages
...
```

#### compact_table

Triggers manual compaction on a table.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> compact_table <keyspace> <table_name> [timeout_in_seconds] (default 20)
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *keyspace*: Specifies the database `ysql.db-name` or keyspace `ycql.keyspace-name`.
- *table_name*: Specifies the table name.
- *timeout_in_seconds*: Specifies duration, in seconds when the cli timeouts waiting for compaction to end. Default value is `20`.

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 compact_table ycql.kong test
Started compaction of table kong.test
Compaction request id: 75c406c1d2964487985f9c852a8ef2a3
Waiting for compaction...
Compaction complete: SUCCESS
```

---

### Backup and snapshot commands

#### list_snapshots

Prints a list of all snapshot IDs,  restoration IDs, and states. Optionally, prints details (including keyspaces, tables, and indexes) in JSON format.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> list_snapshots [ show_details ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- `show_details`: (Optional) Print snapshot details, including the keyspaces, tables, and indexes.

Possible `state` values for creating and restoring snapshots:

- `create_snapshot`: `CREATING`, `COMPLETE`, `DELETING`, `DELETED`, or `FAILED`.
- `restore_snapshot`: `COMPLETE`, `DELETING`, `DELETED`, or `FAILED`.

By default, the `list_snapshot` command prints the current state of the following operations:

- `create_snapshot`: `snapshot_id`, `keyspace`, `table`,  `state`
- `restore_snapshot`: `snapshot_id`, `restoration_id`,  `state`.
- `delete_snapshot`: `snapshot_id`,  `state`.

When `show_details` is included, the `list_snapshot` command prints the following details in JSON format:

- `type`: `NAMESPACE`
  - `id`: `<snapshot_id>` or `<restoration_id>`
  - `data`:
    - `name`:  `"<namespace_name>"`
    - `database_type`: `"YQL_DATABASE_CQL"`
    - `colocated`: `true` or `false`
    - `state`: `"<state>"`
- `type`: `TABLE` <== Use for table or index
  - `id`: `"<table_id>"`  or `"<index_id>"`
  - `data`:
    - `name`: `"<table_name>"` or `"<index_id>"` 
    - `version`: `"<table_version>"`
    - `state`: `"<state>"`
    - `state_msg`: `"<state_msg>"`
    - `next_column_id`: `"<column_id>"`
    - `table_type`: `"YQL_TABLE_TYPE"`
    - `namespace_id`: `"<namespace_id>"`
    - `indexed_table_id` (index only): `<table_id>` 
    - `is_local_index` (index only): `true` or `false`
    - `is_unique_index` (index only):  `true` or `false` 

**Example**

In this example, the optional `show_details` flag is added to generate the snapshot details.

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 list_snapshots show_details
```

Because `show_details` was included, `list_snapshots` prints the details in JSON format, like this:

```json
f566b03b-b85e-41a0-b903-875cd305c1c5 	COMPLETE
{"type":"NAMESPACE","id":"8053dd55d478437cba57d9f67caac154","data":{"name":"yugabyte","database_type":"YQL_DATABASE_CQL","colocated":false,"state":"RUNNING"}}
{"type":"TABLE","id":"a7e940e724ef497ebe94bf69bfe507d9","data":{"name":"tracking1","version":1,"state":"RUNNING","state_msg":"Current schema version=1","next_column_id":13,"table_type":"YQL_TABLE_TYPE","namespace_id":"8053dd55d478437cba57d9f67caac154"}}
{"type":"NAMESPACE","id":"8053dd55d478437cba57d9f67caac154","data":{"name":"yugabyte","database_type":"YQL_DATABASE_CQL","colocated":false,"state":"RUNNING"}}
{"type":"TABLE","id":"b48f4d7695f0421e93386f7a97da4bac","data":{"name":"tracking1_v_idx","version":0,"state":"RUNNING","next_column_id":12,"table_type":"YQL_TABLE_TYPE","namespace_id":"8053dd55d478437cba57d9f67caac154","indexed_table_id":"a7e940e724ef497ebe94bf69bfe507d9","is_local_index":false,"is_unique_index":false}}
```

If `show_details` is not included, `list_snapshots` prints the `snapshot_id` and `state`:

```
f566b03b-b85e-41a0-b903-875cd305c1c5 	COMPLETE
```

#### create_snapshot

Creates a snapshot of the specified tables and their indexes. Prior to v.2.1.8, indexes were not automatically included. You can specify multiple tables, even from different keyspaces.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> create_snapshot <keyspace> <table_name> | <table_id> [<keyspace> <table_name> | <table_id> ]... [flush_timeout_in_seconds]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *keyspace*: The name of the database or keyspace.
- *table_name*: The name of the table name.
- *table_id*: The identifier (ID) of the table.
- *flush_timeout_in_seconds*: Specifies duration, in seconds, before flushing snapshot. Default value is `60`. To skip flushing, set the value to `0`.

When this command runs, a `snapshot_id` is generated and printed.

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 create_snapshot ydb test_tb
```

```
Started flushing table ydb.test_tb
Flush request id: fe0db953a7a5416c90f01b1e11a36d24
Waiting for flushing...
Flushing complete: SUCCESS
Started snapshot creation: 4963ed18fc1e4f1ba38c8fcf4058b295
```

To see if the snapshot creation has finished, run the [`yb-admin list_snapshots`](#list_snapshots) command.

#### restore_snapshot

Restores the specified snapshot, including the tables and indexes. When the operation starts, a `restoration_id` is generated.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> restore_snapshot <snapshot_id>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *snapshot_id*: The identifier (ID) for the snapshot.

**Example**

```sh
yb-admin restore_snapshot  72ad2eb1-65a2-4e88-a448-7ef4418bc469
```

When the restore starts, the `snapshot_id` and the generated `restoration_id` are displayed.

```
Started restoring snapshot: 72ad2eb1-65a2-4e88-a448-7ef4418bc469
Restoration id: 5a9bc559-2155-4c38-ac8b-b6d0f7aa1af6
```

To see if the snapshot was successfully restored, you can run the [`yb-admin list_snapshots`](#list-snapshots) command.

```sh
$ ./bin/yb-admin list_snapshots
```
For the example above, the restore failed, so the following displays:

```
Restoration UUID                 	State
5a9bc559-2155-4c38-ac8b-b6d0f7aa1af6 	FAILED
```

#### export_snapshot

Generates a metadata file for the given snapshot, listing all the relevant internal UUIDs for various objects (table, tablet, etc.).

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> export_snapshot <snapshot_id> <file_name>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *snapshot_id*: The identifier (ID) for the snapshot.
- *file_name*: The name of the the file to contain the metadata. Recommended file extension is `.snapshot`.

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 export_snapshot 4963ed18fc1e4f1ba38c8fcf4058b295 test_tb.snapshot
```

```
Exporting snapshot 4963ed18fc1e4f1ba38c8fcf4058b295 (COMPLETE) to file test_tb.snapshot
Snapshot meta data was saved into file: test_tb.snapshot
```

#### import_snapshot

Imports the specified snapshot metadata file.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> import_snapshot <file_name> [<keyspace> <table_name> [<keyspace> <table_name>]...]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *file_name*: The name of the snapshot file to import
- *keyspace*: The name of the database or keyspace
- *table_name*: The name of the table

{{< note title="Note" >}}

The *keyspace* and the *table* can be different from the exported one.

{{< /note >}}

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 import_snapshot test_tb.snapshot ydb test_tb
```

```
Read snapshot meta file test_tb.snapshot
Importing snapshot 4963ed18fc1e4f1ba38c8fcf4058b295 (COMPLETE)
Target imported table name: ydb.test_tb
Table being imported: ydb.test_tb
Successfully applied snapshot.
Object           	Old ID                           	New ID                          
Keyspace         	c478ed4f570841489dd973aacf0b3799 	c478ed4f570841489dd973aacf0b3799
Table            	ff4389ee7a9d47ff897d3cec2f18f720 	ff4389ee7a9d47ff897d3cec2f18f720
Tablet 0         	cea3aaac2f10460a880b0b4a2a4b652a 	cea3aaac2f10460a880b0b4a2a4b652a
Tablet 1         	e509cf8eedba410ba3b60c7e9138d479 	e509cf8eedba410ba3b60c7e9138d479
Snapshot         	4963ed18fc1e4f1ba38c8fcf4058b295 	4963ed18fc1e4f1ba38c8fcf4058b295
```

#### delete_snapshot

Deletes the snapshot information, usually cleaned up at the end, since this is supposed to be a transient state.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> delete_snapshot <snapshot_id>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *snapshot_id*: The identifier (ID) of the snapshot.

---

### Deployment topology commands

#### Multi-zone and multi-region commands

##### modify_placement_info

Modifies the placement information (cloud, region, and zone) for a deployment.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> modify_placement_info <placement_info> <replication_factor> [ <placement_id> ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *placement_info*: Comma-delimited list of placements for *cloud*.*region*.*zone*. Default value is `cloud1.datacenter1.rack1`.
- *replication_factor*: The number of replicas for each tablet.
- *placement_id*: The identifier of the primary cluster, which can be any unique string. If not set, a randomly-generated ID will be used.

**Example**

```sh
$ ./bin/yb-admin --master_addresses $MASTER_RPC_ADDRS \
    modify_placement_info  \
    aws.us-west.us-west-2a,aws.us-west.us-west-2b,aws.us-west.us-west-2c 3
```

You can verify the new placement information by running the following `curl` command:

```sh
$ curl -s http://<any-master-ip>:7000/cluster-config
```

##### set_preferred_zones

Sets the preferred availability zones (AZs) and regions.

{{< note title="Note" >}}

When nodes in the the "preferred" availability zones and regions are alive and healthy, 
the tablet leaders are placed on nodes in those zones and regions. 
By default, all nodes are eligible to have tablet leaders.
Having all tablet leaders reside in 1 region will 
reduce the number of network hops that the db must do to write transactions and thus increase performance and lowering latency.

{{< /note >}}

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> set_preferred_zones <cloud.region.zone> [<cloud.region.zone>]...
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *cloud.region.zone*: Specifies the cloud, region, and zone. Default value is `cloud1.datacenter1.rack1`.


Suppose you have a deployment with regions: `gcp.us-east4.us-east4-b`, `gcp.asia-northeast1.asia-northeast1-c`,
 `gcp.us-west1.us-west1-c`. Looking at the cluster config:

```sh
$ curl -s http://<any-master-ip>:7000/cluster-config
```

We have a sample config:

```
replication_info {
  live_replicas {
    num_replicas: 3
    placement_blocks {
      cloud_info {
        placement_cloud: "gcp"
        placement_region: "us-west1"
        placement_zone: "us-west1-c"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "gcp"
        placement_region: "us-east4"
        placement_zone: "us-east4-b"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "gcp"
        placement_region: "us-asia-northeast1"
        placement_zone: "us-asia-northeast1-c"
      }
      min_num_replicas: 1
    }
  }
}
```

The following command sets the preferred zone to `gcp.us-west1.us-west1-c`:

```sh
ssh -i $PEM $ADMIN_USER@$MASTER1 \
   ~/master/bin/yb-admin --master_addresses $MASTER_RPC_ADDRS \
    set_preferred_zones  \
    gcp.us-west1.us-west1-c
```

Verify by running the following.

```sh
$ curl -s http://<any-master-ip>:7000/cluster-config
```

Looking again at the cluster config you should see `affinitized_leaders` added:

```
replication_info {
  live_replicas {
    num_replicas: 3
    placement_blocks {
      cloud_info {
        placement_cloud: "gcp"
        placement_region: "us-west1"
        placement_zone: "us-west1-c"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "gcp"
        placement_region: "us-east4"
        placement_zone: "us-east4-b"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "gcp"
        placement_region: "us-asia-northeast1"
        placement_zone: "us-asia-northeast1-c"
      }
      min_num_replicas: 1
    }
  }
  affinitized_leaders {
    placement_cloud: "gcp"
    placement_region: "us-west1"
    placement_zone: "us-west1-c"
  }
}
```

#### Master-follower commands

##### setup_universe_replication

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> setup_universe_replication <producer_universe_uuid> <producer_master_addresses> <comma_separated_list_of_table_ids>
```

- *producer_universe_uuid*: The UUID of the producer universe.
- *producer_master_addresses*: Comma-separated list of master producer addresses.
- *comma_separated_list_of_table_ids*: Comma-separated list of table identifiers (`table_id`).

{{< note title="Tip" >}}

To display a list of tables and their UUID (`table_id`) values, open the **YB-Master UI** (`<master_host>:7000/`) and click **Tables** in the navigation bar.

{{< /note >}}

**Example**

```sh
./bin/yb-admin -master_addresses 127.0.0.11:7100,127.0.0.12:7100,127.0.0.13:7100 setup_universe_replication e260b8b6-e89f-4505-bb8e-b31f74aa29f3 127.0.0.1:7100,127.0.0.2:7100,127.0.0.3:7100 000030a5000030008000000000004000,000030a5000030008000000000004005,dfef757c415c4b2cacc9315b8acb539a
```

##### delete_universe_replication <producer_universe_uuid>

Deletes universe replication for the specified producer universe.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> delete_universe_replication <producer_universe_uuid>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *producer_universe_uuid*: The UUID of the producer universe.

##### set_universe_replication_enabled

Sets the universe replication to be enabled or disabled.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> set_universe_replication_enabled <producer_universe_uuid>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *producer_universe_uuid*: The UUID of the producer universe.
- `0` | `1`: Disabled (`0`) or enabled (`1`). Default is `1`.

#### Read replica commands

##### add_read_replica_placement_info

Add a read replica cluster to the master configuration.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> add_read_replica_placement_info <placement_info> <replication_factor> [ <placement_id> ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *placement_info*: A comma-delimited list of placements for *cloud*.*region*.*zone*. Default value is `cloud1.datacenter1.rack1`.
- *replication_factor*: The number of replicas.
- *placement_id*: The identifier of the read replica cluster, which can be any unique string. If not set, a randomly-generated ID will be used. Primary and read replica clusters must use different placement IDs.

##### modify_read_replica_placement_info

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> modify_read_replica_placement_info <placement_info> <replication_factor> [ <placement_id> ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *placement_info*: A comma-delimited list of placements for *cloud*.*region*.*zone*. Default value is `cloud1.datacenter1.rack1`.
- *replication_factor*: The number of replicas.
- *placement_id*: The identifier of the read replica cluster, which can be any unique string. If not set, a randomly-generated ID will be used. Primary and read replica clusters must use different placement IDs.

##### delete_read_replica_placement_info

Delete the read replica.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> delete_read_replica_placement_info [ <placement_id> ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *placement_id*: The identifier of the read replica cluster, which can be any unique string. If not set, a randomly-generated ID will be used. Primary and read replica clusters must use different placement IDs.

---

### Security commands

#### Encryption at rest commands

For details on using encryption at rest, see [Encryption at rest](../../secure/encryption-at-rest).

#### add_universe_keys_to_all_masters

Sets the contents of `key_path` in-memory on each YB-Master node.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> add_universe_keys_to_all_masters <key_id> <key_path>
```

- *key_id*: Universe-unique identifier (can be any string, such as a string of a UUID) that will be associated to the universe key contained in the contents of `key_path` as a byte[].
- *key_path*:  The path to the file containing the universe key.

{{< note title="Note" >}}

After adding the universe keys to all YB-Master nodes, you can verify the keys exist using the `yb-admin` [`all_masters_have_universe_key_in_memory`](#all-masters-have-universe-key-in-memory) command and enable encryption using the [`rotate_universe_key_in_memory`](#rotate-universe-key-in-memory) command.

{{< /note >}}

#### all_masters_have_universe_key_in_memory

Checks whether the universe key associated with the provided *key_id* exists in-memory on each YB-Master node.

```sh
yb-admin -master_addresses <master-addresses> all_masters_have_universe_key_in_memory <key_id>
```

- *key_id*: Universe-unique identifier (can be any string, such as a string of a UUID) that will be associated to the universe key contained in the contents of `key_path` as a byte[].

#### rotate_universe_key_in_memory

Rotates the in-memory universe key to start encrypting newly-written data files with the universe key associated with the provided `key_id`.

{{< note title="Note" >}}

The [`all_masters_have_universe_key_in_memory`](#all-masters-have-universe-key-in-memory) value must be true for the universe key to be successfully rotated and enabled).

{{< /note >}}

```sh
yb-admin -master_addresses <master-addresses> rotate_universe_key_in_memory <key_id>
```

- *key_id*: Universe-unique identifier (can be any string, such as a string of a UUID) that will be associated to the universe key contained in the contents of `key_path` as a byte[].

#### disable_encryption_in_memory

Disables the in-memory encryption at rest for newly-written data files.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> disable_encryption_in_memory
```

##### is_encryption_enabled

Checks if cluster-wide encryption is enabled.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> is_encryption_enabled
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

Returns message:

```
Encryption status: ENABLED with key id <key_id_2>
```

The new key ID (`<key_id_2>`) should be different from the previous one (`<key_id>`).

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 is_encryption_enabled
```

```
Encryption status: ENABLED with key id <key_id_2>
```

### Change data capture (CDC) commands

#### create_cdc_stream

Creates a change data capture (CDC) stream for the specified table.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> create_cdc_stream <table_id>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *table_id*: The identifier (ID) of the table.

{{< note title="Tip" >}}

To display a list of tables and their UUID (`table_id`) values, open the **YB-Master UI** (`<master_host>:7000/`) and click **Tables** in the navigation bar.

{{< /note >}}

---

### Decommissioning commands

#### get_leader_blacklist_completion

Gets the tablet load move completion percentage for blacklisted nodes.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> get_leader_blacklist_completion
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 get_leader_blacklist_completion
```

#### change_blacklist

Changes the blacklist for YB-TServer servers.

After old YB-TServer servers are terminated, you can use this command to clean up the blacklist.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> change_blacklist [ ADD | REMOVE ] <ip_addr>:<port> [ <ip_addr>:<port> ]...
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- ADD | REMOVE: Adds or removes the specified YB-TServer server.
- *ip_addr:port*: The IP address and port of the YB-TServer.

**Example**

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 change_blacklist ADD node1:9100 node2:9100 node3:9100 node4:9100 node5:9100 node6:9100
```

#### change_leader_blacklist

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> change_leader_blacklist ADD | REMOVE <ip_addr>:<port> [<ip_addr>:<port>]...
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- ADD | REMOVE: Adds or removes nodes from blacklist.
- *ip_addr*: The IP address of the node.
- *port*: The port of the node.

#### leader_stepdown

Forces the YB-TServer leader of the specified tablet to step down.

{{< note title="Note" >}}

Use this command only if recommended by Yugabyte support.

There is a possibility of downtime.

{{< /note >}}

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> leader_stepdown <tablet_id> <dest_ts_uuid>
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- *tablet_id*: The identifier (ID) of the tablet.
- *dest_ts_uuid*: The destination identifier (UUID) for the new YB-TServer leader. To move leadership **from** the current leader, when you do not need to specify a new leader, use `""` for the value. If you want to transfer leadership intentionally **to** a specific new leader, then specify the new leader.

{{< note title="Note" >}}

If specified, `des_ts_uuid` becomes the new leader. If the argument is empty (`""`), then a new leader will be elected automatically. In a future release, this argument will be optional. See GitHub issue [#4722](https://github.com/yugabyte/yugabyte-db/issues/4722)

{{< /note >}}

---

### Rebalancing commands

#### set_load_balancer_enabled

Enables or disables the load balancer.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> set_load_balancer_enabled [ 0 | 1 ]
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.
- `0` | `1`: Enabled (`1`) is the default. To disable, set to `0`.

**Example**

```sh
./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 set_load_balancer_enabled 0
```

#### get_load_balancer_state

Returns the cluster load balancer state.

```sh
yb-admin -master_addresses <master-addresses> get_load_balancer_state
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

#### get_load_move_completion

Checks the percentage completion of the data move.

You can rerun this command periodically until the value reaches `100.0`, indicating that the data move has completed.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> get_load_move_completion
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

{{< note title="Note" >}}

The time needed to complete a data move depends on the following:

- number of tablets and tables
- size of each of those tablets
- SSD transfer speeds
- network bandwidth between new nodes and existing ones

{{< /note >}}

For an example of performing a data move and the use of this command, see [Change cluster configuration](../../manage/change-cluster-config).

**Example**

In the following example, the data move is `66.6` percent done.

```sh
$ ./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 get_load_move_completion
```

Returns the following percentage:

```
66.6
```

#### get_is_load_balancer_idle

Finds out if the load balancer is idle.

**Syntax**

```sh
yb-admin -master_addresses <master-addresses> get_is_load_balancer_idle
```

- *master-addresses*: Comma-separated list of YB-Master hosts and ports. Default value is `localhost:7100`.

**Example**

```sh
./bin/yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 get_is_load_balancer_idle
```
