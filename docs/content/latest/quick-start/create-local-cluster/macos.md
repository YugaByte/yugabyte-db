---
title: Create a local YugabyteDB cluster on macOS
headerTitle: 2. Create a local cluster
linkTitle: 2. Create a local cluster
description: Create a local cluster on macOS in less than five minutes.
aliases:
  - /quick-start/create-local-cluster/
  - /latest/quick-start/create-local-cluster/
menu:
  latest:
    parent: quick-start
    name: 2. Create a local cluster
    identifier: create-local-cluster-1-macos
    weight: 120
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/create-local-cluster/macos" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/create-local-cluster/linux" class="nav-link">
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

{{< note title="Note" >}}

This macOS Quick Start is based on the new [`yugabyted`](../../../reference/configuration/yugabyted/) server. You can refer to the older [`yb-ctl`](../../../admin/yb-ctl/) based instructions in the [v2.1 docs](/v2.1/quick-start/install/linux/).

Note that yugabyted currently supports creating a single-node cluster only. Ability to create multi-node clusters is under [active development](https://github.com/yugabyte/yugabyte-db/issues/2057). 

{{< /note >}}

## 1. Create a local cluster

To create a single-node local cluster with a replication factor (RF) of 1, run the following command.

```sh
$ ./bin/yugabyted start
```

After the cluster is created, clients can connect to the YSQL and YCQL APIs at `localhost:5433` and `localhost:9042` respectively. You can also check `./var/data` to see the data directory and `./var/logs` to see the logs directory.

## 2. Check cluster status

```sh
$ ./bin/yugabyted status
```
```
+--------------------------------------------------------------------------------------------------+
|                                            yugabyted                                             |
+--------------------------------------------------------------------------------------------------+
| Status              : Running                                                                    |
| Web console         : http://127.0.0.1:7000                                                      |
| JDBC                : jdbc:postgresql://127.0.0.1:5433/yugabyte?user=yugabyte&password=yugabyte  |
| YSQL                : bin/ysqlsh                                                                 |
| YCQL                : bin/ycqlsh                                                                 |
| Data Dir            : var/data                                                                   |
| Log Dir             : var/logs                                                                   |
| Universe UUID       : fad6c687-e1dc-4dfd-af4b-380021e19be3                                       |
+--------------------------------------------------------------------------------------------------+
```

## 3. Check cluster status with Admin UI

The [YB-Master Admin UI](../../../reference/configuration/yb-master/#admin-ui) is available at [http://127.0.0.1:7000](http://127.0.0.1:7000) and the [YB-TServer Admin UI](../../../reference/configuration/yb-tserver/#admin-ui) is available at [http://127.0.0.1:9000](http://127.0.0.1:9000). 

### Overview and YB-Master status

The yb-master Admin UI home page shows that you have a cluster with `Replication Factor` of 1 and `Num Nodes (TServers)` as 1. The `Num User Tables` is 0 since there are no user tables created yet. The YugabyteDB version number is also shown for your reference.

![master-home](/images/admin/master-home-binary-rf1.png)

The Masters section highlights the 1 yb-master along with its corresponding cloud, region and zone placement.

### YB-TServer status

Clicking on the `See all nodes` takes us to the Tablet Servers page where you can observe the 1 yb-tserver along with the time since it last connected to this yb-master via regular heartbeats. Since there are no user tables created yet, you can see that the `Load (Num Tablets)` is 0. As new tables get added, new tablets (aka shards) will get automatically created and distributed evenly across all the available tablet servers.

![master-home](/images/admin/master-tservers-list-binary-rf1.png)

{{<tip title="Next step" >}}

[Explore YSQL](../../explore-ysql/)

{{< /tip >}}
