---
title: Explore follower reads on macOS
headerTitle: Follower reads
linkTitle: Follower reads
description: Learn how you can use follower reads to lower read latencies in local YugabyteDB clusters on macOS.
block_indexing: true
menu:
  v2.1:
    identifier: follower-reads-1-macos
    parent: explore
    weight: 235
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/follower-reads/macos" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/follower-reads/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

</ul>

With YugabyteDB, you can use follower reads to lower read latencies since the DB now has less work to do at read time including serving the read from the tablet followers. Follower reads is similar to reading from a cache, which can give more read IOPS with low latency but might have slightly stale yet timeline-consistent data (that is, no out of order is possible). In this tutorial, you will update a single key-value over and over, and read it from the tablet leader. While that workload is running, you will start another workload to read from a follower and verify that you are able to read from a tablet follower.

YugabyteDB also allows you to specify the maximum staleness of data when reading from tablet followers. This means that if the follower hasn't heard from the leader for the specified amount of time, the read request will be forwarded to the leader. This is particularly useful when the tablet follower is located far away from the tablet leader. To enable this feature, you will need to create your cluster with the custom tserver flag `max_stale_read_bound_time_ms`. See [Creating a local cluster with custom flags](../../../admin/yb-ctl/) for instructions on how to do this.

If you haven't installed YugabyteDB yet, do so first by following the [Quick start](../../../quick-start/install/) guide.

## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./bin/yb-ctl destroy
```

Start a new local universe with three nodes and a replication factor (RF) of `3`.

```sh
$ ./bin/yb-ctl --rf 3 create
```

Add one more node.

```sh
$ ./bin/yb-ctl add_node
```

## 2. Write some data

Download the [YugabyteDB workload generator](https://github.com/yugabyte/yb-sample-apps) JAR file (`yb-sample-apps.jar`) by running the following command.

```sh
$ wget https://github.com/yugabyte/yb-sample-apps/releases/download/v1.2.0/yb-sample-apps.jar?raw=true -O yb-sample-apps.jar
```

By default, the YugabyteDB workload generator runs with strong read consistency, where all data is read from the tablet leader. We are going to populate exactly one key with a `10KB` value into the system. Since the replication factor is `3`, this key will get replicated to only three of the four nodes in the universe.

Run the `CassandraKeyValue` workload application to constantly update this key-value, as well as perform reads with strong consistency against the local universe.

```sh
$ java -jar ./yb-sample-apps.jar --workload CassandraKeyValue \
                                    --nodes 127.0.0.1:9042 \
                                    --nouuid \
                                    --num_unique_keys 1 \
                                    --num_threads_write 1 \
                                    --num_threads_read 1 \
                                    --value_size 10240
```

In the above command, we have set the value of `num_unique_keys` to `1`, which means we are overwriting a single key `key:0`. We can verify this using ycqlsh:

```sh
$ ./bin/ycqlsh 127.0.0.1
```

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
```

Now run a query.

```sql
ycqlsh> SELECT k FROM ybdemo_keyspace.cassandrakeyvalue;
```

```
 k
-------
 key:0

(1 rows)
```

## 3. Strongly consistent reads from tablet leaders

When performing strongly consistent reads as a part of the above command, all reads will be served by the tablet leader of the tablet that contains the key `key:0`. If you browse to the <a href='http://127.0.0.1:7000/tablet-servers' target="_blank">tablet-servers</a> page, you will see that all the requests are indeed being served by one tserver:

![Reads from the tablet leader](/images/ce/tunable-reads-leader.png)

## 4. Follower reads from tablet replicas

Stop the workload application above, and then run the following variant of that workload application. This command will do updates to the same key `key:0` which will go through the tablet leader, but it will reads from the replicas.

```sh
$ java -jar ./yb-sample-apps.jar --workload CassandraKeyValue \
                                    --nodes 127.0.0.1:9042 \
                                    --nouuid \
                                    --num_unique_keys 1 \
                                    --num_threads_write 1 \
                                    --num_threads_read 1 \
                                    --value_size 10240 \
                                    --local_reads
```

This can be easily seen by refreshing the <a href='http://127.0.0.1:7000/tablet-servers' target="_blank">tablet-servers</a> page, where you will see that the writes are served by a single YB-TServer that is the leader of the tablet for the key `key:0` while multiple YB-TServers which are replicas serve the reads.

![Reads from the tablet follower](/images/ce/tunable-reads-followers.png)

## 5. Clean up (optional)

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./bin/yb-ctl destroy
```
