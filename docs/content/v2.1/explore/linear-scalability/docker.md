---
title: Explore linear scalability on Docker
headerTitle: Linear scalability
linkTitle: Linear scalability
description: Learn how to scale a local three-node YugabyteDB cluster (on Docker) while a workload is running.
block_indexing: true
menu:
  v2.1:
    identifier: linear-scalability-3-docker
    parent: explore
    weight: 210
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/linear-scalability/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/linear-scalability/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/explore/linear-scalability/docker" class="nav-link active">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/latest/explore/linear-scalability/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

With YugabyteDB, you can add nodes to scale your cluster up very efficiently and reliably in order to achieve more read and write IOPS (input/output operations per second). In this tutorial, you will look at how YugabyteDB can scale while a workload is running. You will run a read-write workload using the prepackaged [YugabyteDB workload generator](https://github.com/yugabyte/yb-sample-apps) against a three-node local cluster with a replication factor of 3, and add nodes to it while the workload is running. Next, you can observe how the cluster scales out by verifying that the number of read and write IOPS are evenly distributed across all the nodes at all times.

## Prerequisite

Install a local YugabyteDB universe on Docker using the steps below.

```sh
mkdir ~/yugabyte && cd ~/yugabyte
wget https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/bin/yb-docker-ctl && chmod +x yb-docker-ctl
docker pull yugabytedb/yugabyte
```

## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./yb-docker-ctl destroy
```

Start a new three-node local cluster with replication factor of `3`. Configure the number of [shards](../../../architecture/concepts/docdb/sharding/) (aka tablets) per table per YB-TServer to `4` so that you can better observe the load balancing during scale-up and scale-down. Each table will now have 4 tablet-leaders in each tserver and with replication factor of `3`, there will be two tablet-followers for each tablet-leader distributed in the two other YB-TServers. So each tserver will have 12 tablets (that is, the sum of 4 tablet-leaders and 8 tablet-followers) per table.

```sh
$ ./yb-docker-ctl create --rf 3 --num_shards_per_tserver 4
```

## 2. Run YugabyteDB workload generator

Pull the [yb-sample-apps](https://github.com/yugabyte/yb-sample-apps) docker container. This container has built-in Java client programs for various workloads, including SQL inserts and updates.

```sh
$ docker pull yugabytedb/yb-sample-apps
```

```sh
$ docker run --name yb-sample-apps --hostname yb-sample-apps --net yb-net yugabytedb/yb-sample-apps --workload SqlInserts \
	--nodes yb-tserver-n1:5433 \
	--num_threads_write 1 \
	--num_threads_read 4
```

The `SqlInserts` workload application prints some stats while running, which is also shown below. 

```
2017-11-20 14:02:48,114 [INFO|...] Read: 9893.73 ops/sec (0.40 ms/op), 233458 total ops  |
                                   Write: 1155.83 ops/sec (0.86 ms/op), 28072 total ops  |  ...

2017-11-20 14:02:53,118 [INFO|...] Read: 9639.85 ops/sec (0.41 ms/op), 281696 total ops  |
                                   Write: 1078.74 ops/sec (0.93 ms/op), 33470 total ops  |  ...
```

## 3. Observe IOPS per node

You can check a lot of the per-node stats by browsing to the <a href='http://localhost:7000/tablet-servers' target="_blank">tablet-servers</a> page. It should look like this. The total read and write IOPS per node are highlighted in the screenshot below. Note that both the reads and the writes are roughly the same across all the nodes indicating uniform usage across the nodes.

![Read and write IOPS with 3 nodes](/images/ce/linear-scalability-3-nodes-docker.png)

## 4. Add node and observe linear scale-out

Add a node to the universe.

```sh
$ ./yb-docker-ctl add_node --num_shards_per_tserver 4
```

Now you should have four nodes. Refresh the <a href='http://localhost:7000/tablet-servers' target="_blank">tablet-servers</a> page to see the stats update. In a short time, you should see the new node performing a comparable number of reads and writes as the other nodes.

![Read and write IOPS with 4 nodes - Rebalancing in progress](/images/ce/linear-scalability-4-nodes-docker.png)

![Read and write IOPS with 4 nodes - Balanced](/images/ce/linear-scalability-4-nodes-balanced-docker.png)

## 5. Remove node and observe linear scale-in

Remove the recently added node from the universe.

```sh
$ ./yb-docker-ctl remove_node 4
```

- Refresh the <a href='http://localhost:7000/tablet-servers' target="_blank">tablet-servers</a> page to see the stats update. The `Time since heartbeat` value for that node will keep increasing. Once that number reaches 60s (i.e. 1 minute), YugabyteDB will change the status of that node from ALIVE to DEAD. Note that at this time the universe is running in an under-replicated state for some subset of tablets.

![Read and write IOPS with 4th node dead](/images/ce/linear-scalability-4-nodes-dead-docker.png)

- After 300s (5 minutes), YugabyteDB's remaining nodes will re-spawn new tablets that were lost with the loss of node 4. Each remaining node's tablet count will increase from 18 to 24.

## 6. Clean up (optional)

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./yb-docker-ctl destroy
```
