---
title: yb-ctl - command line tool for administering local YugabyteDB clusters
headerTitle: yb-ctl
linkTitle: yb-ctl
description: Use the yb-ctl command line tool to administer local YugabyteDB clusters used for development and learning.
menu:
  latest:
    identifier: yb-ctl
    parent: admin
    weight: 2410
aliases:
  - admin/yb-ctl
isTocNested: true
showAsideToc: true
---

The `yb-ctl` utility, located in the bin directory of YugabyteDB home, provides a simple command line interface for administering local clusters used for development and learning. It invokes the [`yb-tserver`](../../reference/configuration/yb-tserver/) and [`yb-master`](../../reference/configuration/yb-master/) servers to perform the necessary orchestration.


{{< note title="Note" >}}

- yb-ctl is meant for managing local clusters only. This means that a single host machine like a local laptop is used to simulate YugabyteDB clusters even though the YugabyteDB cluster can have 3 nodes or more. For creating multi-host clusters, follow the instructions in the [Deploy](../../deploy/) section.

- yb-ctl can manage a cluster if and only if it was initially created via yb-ctl. This means that clusters created through any other means including those in the [Deploy](../../deploy/) section cannot be administered using yb-ctl.

{{< /note >}}

## Syntax

Run `yb-ctl` commands from the YugabyteDB home directory.

```sh
./bin/yb-ctl [ command ] [ flag1, flag2, ... ]
```

### Online help

To display the online help, run `yb-ctl --help` from the YugabyteDB home directory.

```sh
$ ./bin/yb-ctl --help
```

## Commands

##### create

Creates a local YugabyteDB cluster. With no flags, creates a 1-node cluster.

For more details and examples, see [Create a local cluster](#create-a-local-cluster), [Create a cluster across multiple zones, regions, and clouds](#create-a-cluster-across-multiple-zones-regions-and-clouds), and [Create a cluster with custom flags](#create-a-cluster-with-custom-flags).

##### start

Starts the existing cluster or, if not existing, creates and starts the cluster.

##### stop

Stops the cluster, if running.

##### destroy

Destroys the current cluster.

For details and examples, see [Destroy a local cluster](#destroy-a-local-cluster).

##### status

Displays the current status of the cluster.

For details and examples, see [Check cluster status](#check-cluster-status).

##### restart

Restarts the current cluster all at once.

For details and examples, see [Restart a cluster](#restart-a-cluster) and [Restart with custom flags](#restart-with-custom-flags).

##### wipe_restart

Stops the current cluster, wipes all data files and starts the cluster as before (losing all flags).

For details and examples, see [Wipe and restart with placement info flags](#wipe-and-restart-with-placement-info-flags).

##### add_node

Adds a new node to the current cluster. It also takes an optional flag `--master`, which denotes that the server to add is a yb-master.

For details and examples, see [Add nodes](#add-nodes) and [Create a cluster across multiple zones, regions, and clouds](#create-a-cluster-across-multiple-zones-regions-and-clouds).

##### remove_node

Stops a particular node in the running cluster. It also takes an optional flag `--master`, which denotes that the server is a yb-master.

For details and examples, see [Stop and remove nodes](#stop-and-remove-nodes).

##### start_node

Starts a specified node in the running cluster. It also takes an optional flag `--master`, which denotes that the server is a yb-master.

##### stop_node

Stops the specified node in the running cluster. It also takes an optional flag `--master`, which denotes that the server is a yb-master.

For details and examples, see [Stop and remove nodes](#stop-and-remove-nodes).

##### restart_node

Restarts the specified node in a running cluster. It also takes an optional flag `--master`, which denotes that the server is a yb-master.

For details and examples, see [Restart node with placement information](#restart-node-with-placement-information).

##### setup_redis

Enables YugabyteDB support for the Redis-compatible YEDIS API.

For details and examples, see [Initialize the YEDIS API](#initialize-the-yedis-api).

## Flags

##### --help, -h

Shows the help message and then exits.

##### --binary_dir

Specifies the directory in which to find the YugabyteDB `yb-master` and `yb-tserver` binary files.

Default: `<yugabyte-installation-dir>/bin/`

##### --data_dir

Specifies the data directory for YugabyteDB.

Default: `$HOME/yugabyte-data/`

##### --master_flags

Specifies a list of YB-Master flags, separated by commas.

For details and examples, see [Create a cluster with custom flags](#create-a-cluster-with-custom-flags).

##### --tserver_flags

Specifies a list of YB-TServer flags, separated by commas.

For details and examples, see [Create a cluster with custom flags](#create-a-cluster-with-custom-flags).

**Example**

To enable [YSQL authentication](../../secure/authentication/ysql-authentication), you can use the `--tserver_flags` flag to add the `yb-tserver` [`--ysql_enable-auth`](../yb-tserver/#ysql-enable-auth) flag to the `yb-ctl create | start | restart` commands.

```sh
$./bin/yb-ctl create --tserver_flags "ysql_enable_auth=true"
```

##### --placement_info

Specifies the cloud, region, and zone as `cloud.region.zone`, separated by commas.

Default: `cloud1.datacenter1.rack1`

For details and examples, see [Create a cluster across multiple zones, regions, and clouds](#create-a-cluster-across-multiple-zones-regions-and-clouds), [Restart node with placement information](#restart-node-with-placement-information),
and [Wipe and restart with placement info flags](#wipe-and-restart-with-placement-info-flags).

##### --replication_factor, -rf

Specifies the number of replicas for each tablet. This parameter is also known as Replication Factor (RF). Should be an odd number so that a majority consensus can be established. A miniumum value of `3` is needed to create a fault-tolerant cluster since `1` signifies that there is no only 1 replica with no fault tolerance.

This value also sets the default number of YB-Master servers.

Default: `1`

##### --require_clock_sync

Specifies whether YugabyteDB requires clock synchronization between the nodes in the cluster.

Default: `false`

##### --listen_ip

 Specifies the IP address, or port, for a 1-node cluster to listen on. To enable external access of the YugabyteDB APIs and administration ports, set the value to `0.0.0.0`. Note that this flag is not applicable to multi-node clusters.

Default: `127.0.0.1`

##### --num_shards_per_tserver

Number of shards (tablets) to start per tablet server for each table.

Default: `2`

##### --timeout-yb-admin-sec

Timeout, in seconds, for operations that call `yb-admin` and wait on the cluster.

##### --timeout-processes-running-sec

Timeout, in seconds, for operations that wait on the cluster.

##### --verbose

Flag to log internal debug messages to `stderr`.

## Create a local cluster

To create a local YugabyteDB cluster for development and learning, use the `yb-ctl create` command.

In order to ensure that all of the replicas for a given tablet can be placed on different nodes, the number of nodes created with the initial create command is always equal to the replication factor.  To expand or shrink the cluster, use the [`add_node`](#add-nodes) and [`remove_node`](#stop-remove-nodes) commands.

Each of these initial nodes run a `yb-tserver` server and a `yb-master` server. Note that the number of YB-Master servers in a cluster must equal the replication factor for the cluster to be considered operating normally.

### Create a local 1-node cluster with replication factor of 1

```sh
$ ./bin/yb-ctl create
```

Note that the default replication factor is 1.

### Create a 4-node cluster with replication factor of 3

First create 3-node cluster with replication factor of `3`.

```sh
$ ./bin/yb-ctl --rf 3 create
```

Use `yb-ctl add_node` command to add a node and make it a 4-node cluster.

```sh
$ ./bin/yb-ctl add_node
```

### Create a 5-node cluster with replication factor of 5

```sh
$ ./bin/yb-ctl --rf 5 create
```

## Default directories for local clusters

YugabyteDB clusters created with the `yb-ctl` utility are created locally on the same host and simulate a distributed multi-host cluster.

### Data directory

YugabyteDB cluster data is installed in `$HOME/yugabyte-data/`, containing the following:

```sh
cluster_config.json
initdb.log
node-#/
node-#/disk-#/
```

#### Node directories

For each simulated YugabyteDB node, a `yugabyte-data` subdirectory, named `node-#` (where # is the number of the node), is created.

Example: `/yugabyte-data/node-#/`

Each `node-#` directory contains the following:

```sh
yugabyte-data/node-#/disk-#/
```

#### Disk directories

For each simulated disk, a `disk-#` subdirectory is created in each `/yugabyte-data/node-#` directory.

Each `disk-#` directory contains the following:

```sh
master.err
master.out
pg_data/
tserver.err
tserver.out
yb-data/
```

### Logs

YB-Master logs are added in the following location:

```sh
yugabyte-data/node-#/disk-#/yb-data/master.out
yugabyte-data/node-#/disk-#/yb-data/master/logs
```

YB-TServer logs are added in the following location:

```sh
yugabyte-data/node-#/disk-#/yb-data/tserver.out
yugabyte-data/node-#/disk-#/yb-data/tserver/logs
```

## Enable external access

There are essentially two modes with yb-ctl.

- 1-node RF1 cluster where the bind IP address for all ports can be bound to `0.0.0.0` using the `listen_ip` flag. This is the mode you use if you want to have external access for the database APIs and admin UIs.

```sh
$ ./bin/yb-ctl create --listen_ip=0.0.0.0
```

- Multi-node (say 3-node RF3) cluster where the bind IP addresses are the loopback IP addresses since binding to `0.0.0.0` is no longer possible. Hence, this mode is only meant for internal access.

## Check cluster status

To get the status of your local cluster, including the Admin UI URLs for the YB-Master and YB-TServer, run the `yb-ctl status` command.

```sh
$ ./bin/yb-ctl status
```

Following is the output shown for a 3-node RF3 cluster.

```
----------------------------------------------------------------------------------------------------
| Node Count: 3 | Replication Factor: 3                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/yugabyte                                  |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /Users/testuser12/yugabyte-data                                            |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 1: yb-tserver (pid 27389), yb-master (pid 27380)                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/yugabyte                                  |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| data-dir[0]         : /Users/testuser12/yugabyte-data/node-1/disk-1/yb-data                      |
| yb-tserver Logs     : /Users/testuser12/yugabyte-data/node-1/disk-1/yb-data/tserver/logs         |
| yb-master Logs      : /Users/testuser12/yugabyte-data/node-1/disk-1/yb-data/master/logs          |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 2: yb-tserver (pid 27392), yb-master (pid 27383)                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.2:5433/yugabyte                                  |
| YSQL Shell          : bin/ysqlsh -h 127.0.0.2                                                    |
| YCQL Shell          : bin/ycqlsh 127.0.0.2                                                        |
| YEDIS Shell         : bin/redis-cli -h 127.0.0.2                                                 |
| data-dir[0]         : /Users/testuser12/yugabyte-data/node-2/disk-1/yb-data                      |
| yb-tserver Logs     : /Users/testuser12/yugabyte-data/node-2/disk-1/yb-data/tserver/logs         |
| yb-master Logs      : /Users/testuser12/yugabyte-data/node-2/disk-1/yb-data/master/logs          |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 3: yb-tserver (pid 27395), yb-master (pid 27386)                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.3:5433/yugabyte                                  |
| YSQL Shell          : bin/ysqlsh -h 127.0.0.3                                                    |
| YCQL Shell          : bin/ycqlsh 127.0.0.3                                                        |
| YEDIS Shell         : bin/redis-cli -h 127.0.0.3                                                 |
| data-dir[0]         : /Users/testuser12/yugabyte-data/node-3/disk-1/yb-data                      |
| yb-tserver Logs     : /Users/testuser12/yugabyte-data/node-3/disk-1/yb-data/tserver/logs         |
| yb-master Logs      : /Users/testuser12/yugabyte-data/node-3/disk-1/yb-data/master/logs          |
----------------------------------------------------------------------------------------------------
```

## Start and stop an existing cluster

Start the existing cluster, or create and start a cluster (if one doesn't exist) by running the `yb-ctl start` command.

```sh
$ ./bin/yb-ctl start
```

Stop a cluster so that you can start it later by running the `yb-ctl stop` command.

```sh
$ ./bin/yb-ctl stop
```

## Add and remove nodes

### Add nodes

This will start a new YB-TServer server and give it a new `node_id` for tracking purposes.

```sh
$ ./bin/yb-ctl add_node
```

### Stop and remove nodes

We can stop a node by executing the `yb-ctl stop` command. The command takes the `node_id` of the node that has to be removed as input. Stop node command expects a node id which denotes the index of the server that needs to be stopped. It also takes an optional flag `--master`, which denotes that the server is a yb-master.

```sh
$ ./bin/yb-ctl stop_node 3
```

We can also pass an optional flag `--master`, which denotes that the server is a yb-master.

```sh
$ ./bin/yb-ctl stop_node 3 --master
```

Currently `stop_node` and `remove_node` implement exactly the same behavior. So they can be used interchangeably.

## Test failure of a node

You can test the failure of a node in a 3-node RF3 cluster by killing 1 instance of yb-tserver and 1 instance of yb-master by using the following commands.

```sh
./bin/yb-ctl destroy
./bin/yb-ctl --rf 3 create
./bin/yb-ctl stop_node 3
./bin/yb-ctl stop_node 3 --master
./bin/yb-ctl start_node 3
./bin/yb-ctl start_node 3 --master
```

The command `./bin/yb-ctl start_node 3` will start yb-tserver3. However, it will throw an error even though the command will succeed. This is because there are only 2 yb-masters present in the cluster at this point. This is not an error in the cluster configuration but rather a warning to highlight that the cluster is under-replicated and does not have enough yb-masters to ensure continued fault tolerance. Following [GitHub issue](https://github.com/yugabyte/yugabyte-db/issues/4156) tracks the work to convert this error into a user-friendly warning.

## Initialize the YEDIS API

The `setup_redis` command to initialize YugabyteDB's Redis-compatible YEDIS API.

```sh
$ ./bin/yb-ctl setup_redis
```

## Destroy a local cluster

This command stops all the nodes and deletes the data directory of the cluster.

```sh
$ ./bin/yb-ctl destroy
```

## Advanced commands

### Create a cluster across multiple zones, regions, and clouds

You can pass the placement information for nodes in a cluster from the command line. The placement information is provided as a set of (cloud, region, zone) tuples separated by commas. Each cloud, region and zone entry is separated by dots.

```sh
$ ./bin/yb-ctl --rf 3 create --placement_info "cloud1.region1.zone1,cloud2.region2.zone2"
```

The total number of placement information entries cannot be more than the replication factor (this is because you would not be able to satisfy the data placement constraints for this replication factor). If the total number of placement information entries is lesser than the replication factor, the placement information is passed down to the node in a round robin approach.

To add a node:

```sh
$ ./bin/yb-ctl add_node --placement_info "cloud1.region1.zone1"
```

### Create a local cluster with custom flags

When you use `yb-ctl`, you can pass "custom" flags (flags unavailable directly in `yb-ctl`) to the YB-Master and YB-TServer servers.

```sh
$ ./bin/yb-ctl --rf 1 create --master_flags "log_cache_size_limit_mb=128,log_min_seconds_to_retain=20,master_backup_svc_queue_length=70" --tserver_flags "log_inject_latency=false,log_segment_size_mb=128,raft_heartbeat_interval_ms=1000"
```

To add a node with custom YB-TServer flags:

```sh
$ ./bin/yb-ctl add_node --tserver_flags "log_inject_latency=false,log_segment_size_mb=128"
```

To add a node with custom YB-Master flags:

```sh
$ ./bin/yb-ctl add_node --master_flags "log_cache_size_limit_mb=128,log_min_seconds_to_retain=20"
```

### Restart a cluster

The `yb-ctl restart` command can be used to restart a cluster. Please note that if you restart the cluster, all custom defined flags and placement information will be lost. Nevertheless, you can pass the placement information and custom flags in the same way as they are passed in the `yb-ctl create` command.

```sh
$ ./bin/yb-ctl restart
```

- Restart with cloud, region and zone flags

```sh
$ ./bin/yb-ctl restart --placement_info "cloud1.region1.zone1" 
```

### Restart with custom flags

```sh
$ ./bin/yb-ctl restart --master_flags "log_cache_size_limit_mb=128,log_min_seconds_to_retain=20,master_backup_svc_queue_length=70" --tserver_flags "log_inject_latency=false,log_segment_size_mb=128,raft_heartbeat_interval_ms=1000"
```

### Restart a node

The `yb-ctl restart` first stops the node and then starts it again. At this point of time, the node is not decommissioned from the cluster. Thus one of the primary advantages of this command is that it can be used to clear old flags and pass in new ones. Just like  create, you can pass the cloud/region/zone and custom flags in the `yb-ctl restart` command.

```sh
$ ./bin/yb-ctl restart_node 2

```

#### Restart yb-master on a node

```sh
$ ./bin/yb-ctl restart_node 2 --master
```

#### Restart node with placement information

```sh
$ ./bin/yb-ctl restart_node 2 --placement_info "cloud1.region1.zone1"
```

#### Restart node with flags

```sh
$ ./bin/yb-ctl restart_node 2 --master --master_flags "log_cache_size_limit_mb=128,log_min_seconds_to_retain=20"
```

### Wipe and restart a cluster

The `yb-ctl wipe_restart` command stops all the nodes, removes the underlying data directories, and then restarts with the same number of nodes that you had in your previous configuration.

Just like the `yb-ctl restart` command, the custom-defined flags and placement information will be lost during `wipe_restart`, though you can pass placement information and custom flags in the same way as they are passed in the `yb-ctl create` command.

```sh
$ ./bin/yb-ctl wipe_restart
```

#### Wipe and restart with placement info flags

```sh
$ ./bin/yb-ctl wipe_restart --placement_info "cloud1.region1.zone1" 
```

#### Wipe and restart with custom flags

```sh
$ ./bin/yb-ctl wipe_restart --master_flags "log_cache_size_limit_mb=128,log_min_seconds_to_retain=20,master_backup_svc_queue_length=70" --tserver_flags "log_inject_latency=false,log_segment_size_mb=128,raft_heartbeat_interval_ms=1000"
```
