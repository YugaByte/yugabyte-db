---
title: Create a local YugabyteDB cluster on Linux
headerTitle: 2. Create a local YugabyteDB cluster
linkTitle: 2. Create a local cluster
description: Create a local YugabyteDB cluster on Linux in less than five minutes.
block_indexing: true
menu:
  v2.1:
    parent: quick-start
    name: 2. Create a local cluster
    identifier: create-local-cluster-2-linux
    weight: 120
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/create-local-cluster/macos" class="nav-link ">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/create-local-cluster/linux" class="nav-link active">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/create-local-cluster/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/create-local-cluster/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

## 1. Create a local cluster

You can use the [`yb-ctl`](../../../admin/yb-ctl/) utility, located in the `bin` directory of the YugabyteDB package, to create and administer a local cluster. The default data directory is `$HOME/yugabyte-data`. You can change the location of the data directory by using the [`--data_dir`](../../../admin/yb-ctl/#data-dir) flag.

To create a 1-node or 3-node local cluster, follow the steps below. For details on using the `yb-ctl create` command and the cluster configuration, see [Create a local cluster](../../../admin/yb-ctl/#create-cluster) in the CLI reference.

### Create a 1-node cluster with RF of 1

To create a 1-node cluster with a replication factor (RF) of 1, run the following `yb-ctl create` command. 

```sh
$ ./bin/yb-ctl create
```

Optionally, you can enable *external access* to the YugabyteDB APIs and administration ports by setting the [`--listen_ip`](../../../admin/yb-ctl/#listen-ip) flag to `0.0.0.0`. Note that this option is limited to 1-node (with a RF of `1`) clusters.

```sh
$ ./bin/yb-ctl create --listen_ip=0.0.0.0
```

### Create a 3-node cluster with RF of 3

To run a distributed SQL cluster locally for development and testing, you can create a 3-node cluster with RF of 3 by executing the following command.

```sh
$ ./bin/yb-ctl --rf 3 create
```

Note that in this 3-node mode, the bind IP address by default for all ports is the individual loopback address (that you setup in the previous step). In this mode you will not be able to externally access the database APIs and admin UIs because `0.0.0.0` remains unbound.

You can now check `$HOME/yugabyte-data` to see the `node-<id>` directories (where `<id>` represents the `node_id` of the node). Inside each node directory is a subdirectory, named `disk-1`, that simulates a disk. Note that the IP address of `node-<id>` is by default set to `127.0.0.<id>`.

## 2. Check cluster status with yb-ctl

To see the `yb-master` and `yb-tserver` processes running locally, run the `yb-ctl status` command.

### Example

For a 1-node cluster, the `yb-ctl status` command will show that you have 1 `yb-master` process and 1 `yb-tserver` process running on the localhost. For details about the roles of these processes in a YugabyteDB cluster (aka Universe), see [Universe](../../../architecture/concepts/universe/).

```sh
$ ./bin/yb-ctl status
```

```
----------------------------------------------------------------------------------------------------
| Node Count: 1 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /Users/yugabyte/yugabyte-data                                              |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 1: yb-tserver (pid 20696), yb-master (pid 20693)                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| data-dir[0]         : /Users/yugabyte/yugabyte-data/node-1/disk-1/yb-data                        |
| yb-tserver Logs     : /Users/yugabyte/yugabyte-data/node-1/disk-1/yb-data/tserver/logs           |
| yb-master Logs      : /Users/yugabyte/yugabyte-data/node-1/disk-1/yb-data/master/logs            |
----------------------------------------------------------------------------------------------------
```

## 3. Check cluster status with Admin UI

Node 1's [YB-Master Admin UI](../../../reference/configuration/yb-master/#admin-ui) is available at `http://127.0.0.1:7000` and the [YB-TServer Admin UI](../../../reference/configuration/yb-tserver/#admin-ui) is available at `http://127.0.0.1:9000`. If you created a multi-node cluster, you can visit the other nodes' Admin UIs by using their corresponding IP addresses.

### Overview and YB-Master status

Node 1's master Admin UI home page shows that you have a cluster (aka a Universe) with `Replication Factor` of 1 and `Num Nodes (TServers)` as 1. The `Num User Tables` is 0 since there are no user tables created yet. YugabyteDB version number is also shown for your reference.

![master-home](/images/admin/master-home-binary-rf1.png)

The Masters section highlights the 1 yb-master along with its corresponding cloud, region and zone placement.

### YB-TServer status

Clicking on the `See all nodes` takes us to the Tablet Servers page where you can observe the 1 tserver along with the time since it last connected to this master via regular heartbeats. Since there are no user tables created yet, you can see that the `Load (Num Tablets)` is 0. As new tables get added, new tablets (aka shards) will get automatically created and distributed evenly across all the available tablet servers.

![master-home](/images/admin/master-tservers-list-binary-rf1.png)

{{<tip title="Next step" >}}

[Explore YSQL](../../explore-ysql/)

{{< /tip >}}
