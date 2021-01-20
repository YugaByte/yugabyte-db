---
title: Explore fault tolerance on Linux
headerTitle: Fault tolerance
linkTitle: Fault tolerance
description: Simulate fault tolerance and resilience in a local three-node YugabyteDB cluster on Linux.
aliases:
  - /stable/explore/fault-tolerance-linux/
block_indexing: true
menu:
  stable:
    identifier: fault-tolerance-2-linux
    parent: explore
    weight: 215
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/explore/fault-tolerance/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/stable/explore/fault-tolerance/linux" class="nav-link active">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/stable/explore/fault-tolerance/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/stable/explore/fault-tolerance/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

YugabyteDB can automatically handle failures and therefore provides [high availability](../../../architecture/core-functions/high-availability/). You will create YSQL tables with a replication factor (RF) of `3` that allows a [fault tolerance](../../../architecture/concepts/docdb/replication/) of `1`. This means the cluster will remain available for both reads and writes even if one node fails. However, if another node fails bringing the number of failures to two, then writes will become unavailable on the cluster in order to preserve data consistency.

This tutorial uses the [yb-ctl](../../../admin/yb-ctl) local cluster management utility.

## 1. Create a universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./bin/yb-ctl destroy
```

Start a new local three-node cluster with a replication factor (RF) of `3`.

```sh
$ ./bin/yb-ctl --rf 3 create
```

## 2. Run the sample key-value app

Download the YugabyteDB workload generator JAR file (`yb-sample-apps.jar`).

```sh
$ wget https://github.com/yugabyte/yb-sample-apps/releases/download/1.3.1/yb-sample-apps.jar?raw=true -O yb-sample-apps.jar 
```

Run the `SqlInserts` workload against the local universe using the following command.

```sh
$ java -jar ./yb-sample-apps.jar --workload SqlInserts \
                                    --nodes 127.0.0.1:5433 \
                                    --num_threads_write 1 \
                                    --num_threads_read 4
```

The `SQLInserts` workload prints some stats while running, which is also shown below. You can read more details about the output of the workload applications at the [YugabyteDB workload generator](https://github.com/yugabyte/yb-sample-apps).

```
2018-05-10 09:10:19,538 [INFO|...] Read: 8988.22 ops/sec (0.44 ms/op), 818159 total ops  |  Write: 1095.77 ops/sec (0.91 ms/op), 97120 total ops  | ... 
2018-05-10 09:10:24,539 [INFO|...] Read: 9110.92 ops/sec (0.44 ms/op), 863720 total ops  |  Write: 1034.06 ops/sec (0.97 ms/op), 102291 total ops  | ...
```

## 3. Observe even load across all nodes

You can check a lot of the per-node stats by browsing to the <a href='http://127.0.0.1:7000/tablet-servers' target="_blank">tablet-servers</a> page. It should look like this. The total read and write IOPS per node are highlighted in the screenshot below. Note that both the reads and the writes are roughly the same across all the nodes indicating uniform usage across the nodes.

![Read and write IOPS with 3 nodes](/images/ce/pgsql-fault-tolerance-3-nodes.png)

## 4. Remove a node and observe continuous write availability

Remove a node from the universe.

```sh
$ ./bin/yb-ctl remove_node 3
```

Refresh the <a href='http://127.0.0.1:7000/tablet-servers' target="_blank">tablet-servers</a> page to see the stats update. The `Time since heartbeat` value for that node will keep increasing. Once that number reaches 60s (1 minute), YugabyteDB will change the status of that node from `ALIVE` to `DEAD`. Note that at this time the universe is running in an under-replicated state for some subset of tablets.

![Read and write IOPS with 3rd node dead](/images/ce/pgsql-fault-tolerance-1-node-dead.png)

## 4. Remove another node and observe write unavailability

Remove another node from the universe.

```sh
$ ./bin/yb-ctl remove_node 2
```

Refresh the <a href='http://127.0.0.1:7000/tablet-servers' target="_blank">tablet-servers</a> page to see the stats update. Writes are now unavailable but reads can continue to be served for whichever tablets available on the remaining node.

![Read and write IOPS with 2nd node removed](/images/ce/pgsql-fault-tolerance-2-nodes-dead.png)

## 6. [Optional] Clean up

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./bin/yb-ctl destroy
```
