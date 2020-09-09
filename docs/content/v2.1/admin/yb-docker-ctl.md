---
title: yb-docker-ctl - command line tool for administering local Docker-based clusters
headerTitle: yb-docker-ctl
linkTitle: yb-docker-ctl
description: Use the yb-docker-ctl command line tool to administer local Docker-based YugabyteDB clusters for development and learning.
block_indexing: true
menu:
  v2.1:
    identifier: yb-docker-ctl
    parent: admin
    weight: 2420
isTocNested: true
showAsideToc: true
---

The `yb-docker-ctl` utility provides a simple command line interface (CLI), or shell, for administering a local Docker-based cluster for development and learning. It manages the [YB-Master](../yb-master/) and [YB-TServer](../yb-tserver/) containers to perform the necessary administration.

## Download

```sh
$ mkdir ~/yugabyte && cd ~/yugabyte
```

```sh
$ wget https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/bin/yb-docker-ctl && chmod +x yb-docker-ctl
```

## Online help

Run `yb-docker-ctl --help` to display the online help.

```sh
$ ./yb-docker-ctl -h
```

## Syntax

```sh
yb-docker-ctl [ command ] [ arguments ]

```

## Commands

### create

Creates a local YugabyteDB cluster.

### add_node

Adds a new local YugabyteDB cluster node.

### status

Displays the current status of the local YugabyteDB cluster.

### destroy

Destroys the local YugabyteDB cluster.

### stop_node

Stops the specified local YugabyteDB cluster node.

### start_node

Starts the specified local YugabyteDB cluster node.

### stop

Stops the local YugabyteDB cluster so that it can be started later.

### start

Starts the local YugabyteDB cluster, if it already exists.

### remove_node

Stops the specified local YugabyteDB cluster node.

## Flags

### --help, -h

Displays the online help and then exits.

### --tag

Use with `create` and `add_node` commands to specify a specific Docker image tag (version). If not included, then latest Docker image is used.

## Create a cluster

Use the `yb-docker-ctl create` command to create a local Docker-based cluster for development and learning.

The number of nodes created when you use the `yb-docker-ctl create` command is always equal to the replication factor (RF), ensuring that all of the replicas for a given tablet can be placed on different nodes. With the [`add_node`](#add-a-node) and [`remove_node`](#remove-a-node) commands, the size of the cluster can thereafter be expanded or shrunk as needed.

### Specify a docker image tag

By default, the `create` and `add_node` commands pull the latest Docker Hub `yugabytedb/yugabyte` image to create clusters or add nodes.

To pull an earlier Docker image tag (version), add the `--tag <tag-id>` flag to use an earlier release.

In the following example, a 1-node YugabyteDB cluster is created using the earlier v1.3.2.1 release that has a tag of `1.3.2.1-b2`.

```
$yb-docker-ctl create --tag 1.3.2.1-b2
```

To get the correct tag value, see the [Docker Hub listing of tags for `yugabytedb/yugabyte`](https://hub.docker.com/r/yugabytedb/yugabyte/tags).

### Create a 1-node local cluster with replication factor of 1

To create a 1-node local YugabyteDB cluster for development and learning, run the default `yb-docker-ctl` command. By default, this creates a 1-node cluster with a replication factor (RF) of 1. Note that the `yb-docker-ctl create` command pulls the latest `yugabytedb/yugabyte` image at the outset, in case the image has not yet downloaded or is not the latest version.

```sh
$ ./yb-docker-ctl create
```

### Create a 3-node local cluster with replication factor of 3

When you create a 3-node local Docker-based cluster using the `yb-docker-ctl create` command, each of the initial nodes run a `yb-tserver` process and a `yb-master` process. Note that the number of YB-Masters in a cluster has to equal to the replication factor (RF) for the cluster to be considered as operating normally and the number of YB-TServers is equal to be the number of nodes.

To create a 3-node local Docker-based cluster for development and learning, run the following `yb-docker-ctl` command.

```sh
$ ./yb-docker-ctl create --rf 3
```

```
docker run --name yb-master-n1 --privileged -p 7000:7000 --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-master --fs_data_dirs=/mnt/disk0,/mnt/disk1 --master_addresses=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-master-n1:7100
Adding node yb-master-n1
docker run --name yb-master-n2 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-master --fs_data_dirs=/mnt/disk0,/mnt/disk1 --master_addresses=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-master-n2:7100
Adding node yb-master-n2
docker run --name yb-master-n3 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-master --fs_data_dirs=/mnt/disk0,/mnt/disk1 --master_addresses=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-master-n3:7100
Adding node yb-master-n3
docker run --name yb-tserver-n1 --privileged -p 9000:9000 -p 9042:9042 -p 6379:6379 --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-tserver-n1:9100
Adding node yb-tserver-n1
docker run --name yb-tserver-n2 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-tserver-n2:9100
Adding node yb-tserver-n2
docker run --name yb-tserver-n3 --privileged --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=yb-master-n1:7100,yb-master-n2:7100,yb-master-n3:7100 --rpc_bind_addresses=yb-tserver-n3:9100
Adding node yb-tserver-n3
PID        Type       Node                 URL                       Status          Started At
11818      tserver    yb-tserver-n3        http://172.19.0.7:9000    Running         2017-11-28T23:33:00.369124907Z
11632      tserver    yb-tserver-n2        http://172.19.0.6:9000    Running         2017-11-28T23:32:59.874963849Z
11535      tserver    yb-tserver-n1        http://172.19.0.5:9000    Running         2017-11-28T23:32:59.444064946Z
11350      master     yb-master-n3         http://172.19.0.4:9000    Running         2017-11-28T23:32:58.899308826Z
11231      master     yb-master-n2         http://172.19.0.3:9000    Running         2017-11-28T23:32:58.403788411Z
11133      master     yb-master-n1         http://172.19.0.2:9000    Running         2017-11-28T23:32:57.905097927Z
```

### Create a 5-node local cluster with replication factor of 5

```sh
$ ./yb-docker-ctl create --rf 5
```

## Check cluster status

Get the status of your local cluster, including the URLs for the Admin UI for each YB-Master and YB-TServer.

```sh
$ ./yb-docker-ctl status
```

```
PID        Type       Node                 URL                       Status          Started At
11818      tserver    yb-tserver-n3        http://172.19.0.7:9000    Running         2017-11-28T23:33:00.369124907Z
11632      tserver    yb-tserver-n2        http://172.19.0.6:9000    Running         2017-11-28T23:32:59.874963849Z
11535      tserver    yb-tserver-n1        http://172.19.0.5:9000    Running         2017-11-28T23:32:59.444064946Z
11350      master     yb-master-n3         http://172.19.0.4:9000    Running         2017-11-28T23:32:58.899308826Z
11231      master     yb-master-n2         http://172.19.0.3:9000    Running         2017-11-28T23:32:58.403788411Z
11133      master     yb-master-n1         http://172.19.0.2:9000    Running         2017-11-28T23:32:57.905097927Z
```

## Add a node

Add a new node to the cluster. This will start a new `yb-tserver` process and give it a new `node_id` for tracking purposes.

```sh
$ ./yb-docker-ctl add_node
```

```
docker run --name yb-tserver-n4 --net yb-net --detach yugabytedb/yugabyte:latest /home/yugabyte/yb-tserver --fs_data_dirs=/mnt/disk0,/mnt/disk1 --tserver_master_addrs=04:7100,04:7100,04:7100 --rpc_bind_addresses=yb-tserver-n4:9100
Adding node yb-tserver-n4
```

## Remove a node

Remove a node from the cluster by executing the following command. The command takes the `node_id` of the node to be removed as input.

### Help

```sh
$ ./yb-docker-ctl remove_node --help
```

```
usage: yb-docker-ctl remove_node [-h] node

positional arguments:
  node_id        Index of the node to remove

optional arguments:
  -h, --help  show this help message and exit
```

### Example

```sh
$ ./yb-docker-ctl remove_node 3
```

```
Stopping node :yb-tserver-n3
```

## Destroy cluster

The `yb-docker-ctl destroy` command below destroys the local cluster, including deletion of the data directories.

```sh
$ ./yb-docker-ctl destroy

```

## Upgrade container image

The following `docker pull` command below upgrades the Docker image of YugabyteDB to the latest version.

```sh
$ docker pull yugabytedb/yugabyte
```
