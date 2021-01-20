---
title: Sharding Data
headerTitle: Sharding Data Across Nodes
linkTitle: Sharding Data Across Nodes
description: Sharding Data Across Nodes in YugabyteDB.
headcontent: Sharding Data Across Nodes in YugabyteDB.
menu:
  latest:
    name: Sharding Data
    identifier: explore-transactions-sharding-data-1-ysql
    parent: explore-scalability
    weight: 240
isTocNested: true
showAsideToc: true
---

<!--
<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/transactions/scaling-transactions/" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/explore/transactions/distributed-transactions-ycql/" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>
-->

YugabyteDB automatically splits user tables into multiple shards, called *tablets*. The primary key for each row in the table uniquely determines the tablet the row lives in. For data distribution purposes, a hash based partitioning scheme is used. Read more about [how sharding works](../../../architecture/docdb/sharding/) in YugabyteDB.

By default, YugabyteDB creates eight tablets per node in the cluster for each table and automatically distributes the data across the various tablets, which in turn are distributed evenly across the nodes. In this tutorial, you will explore how automatic sharding is done internally for tables. The system Redis table works in an identical manner.

We will explore automatic sharding inside YugabyteDB by creating these tables:

- Use a replication factor (RF) of `1`. This will make it easier to understand how automatic sharding is achieved independent of data replication.
- Insert entries one by one, and examine which how the data gets distributed across the various nodes.

This tutorial uses the [yb-ctl](../../../admin/yb-ctl) local cluster management utility.

## 1. Create a universe

If you have a currently running local universe, destroy it using the following.

```sh
$ ./bin/yb-ctl destroy
```

Start a new local universe with a replication factor of 1 (RF=1). We are passing the following flags:

- `--rf 1` This creates a universe with a replication factor of 1.
- `--num_shards_per_tserver 4`  This flag controls the total number of tablets (or partitions) when creating a new table. By setting the value to `4`, 12 tablets will be created on a 3-node cluster.
- `--tserver_flags "memstore_size_mb=1"` This sets the total size of memstores on the tablet-servers to `1MB`. This will force a flush of the data to disk when a value greater than 1MB is added, so that you can observe which tablets the data is written to.


{{< note title="Warning" >}}
Setting `memstore_size` to such a low value is not recommended in production and is only being used here to illustrate the point by forcing flushes to happen more quickly.
{{< /note >}}


You can do this as shown below.

```sh
$ ./bin/yb-ctl --rf 1 --num_shards_per_tserver 4 create \
             --tserver_flags "memstore_size_mb=1"
```

This example creates a universe with one node. Now, let's add two more nodes to make this a 3-node, rf=1 universe. We need to pass the memstore size flag to each of the added YB-TServer servers. You can do that by running the following:

```sh
$ ./bin/yb-ctl add_node --tserver_flags "memstore_size_mb=1"
```

```sh
$ ./bin/yb-ctl add_node --tserver_flags "memstore_size_mb=1"
```

We can check the status of the cluster to confirm that you have three YB-TServer servers.

```sh
$ ./bin/yb-ctl status
```

```
----------------------------------------------------------------------------------------------------
| Node Count: 3 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/yugabyte                                 |
| YSQL                : ./bin/ysqlsh                                                               |
| YCQL                : ./bin/ycqlsh 127.0.0.1 9042                                                 |
| YEDIS               : ./bin/redis-cli -h 127.0.0.1 -p 6379                                       |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /Users/schoudhury/yugabyte-data                                            |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 1: yb-tserver (pid 72053), yb-master (pid 72050)                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL                : ./bin/ysqlsh                                                               |
| YCQL                : ./bin/ycqlsh 127.0.0.1 9042                                                 |
| YEDIS               : ./bin/redis-cli -h 127.0.0.1 -p 6379                                       |
| data-dir[0]         : /Users/schoudhury/yugabyte-data/node-1/disk-1/yb-data                      |
| TServer Logs        : /Users/schoudhury/yugabyte-data/node-1/disk-1/yb-data/tserver/logs         |
| Master Logs         : /Users/schoudhury/yugabyte-data/node-1/disk-1/yb-data/master/logs          |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 2: yb-tserver (pid 72128)                                                                   |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL                : ./bin/ysqlsh                                                               |
| YCQL                : ./bin/ycqlsh 127.0.0.2 9042                                                 |
| YEDIS               : ./bin/redis-cli -h 127.0.0.2 -p 6379                                       |
| data-dir[0]         : /Users/schoudhury/yugabyte-data/node-2/disk-1/yb-data                      |
| TServer Logs        : /Users/schoudhury/yugabyte-data/node-2/disk-1/yb-data/tserver/logs         |
----------------------------------------------------------------------------------------------------
----------------------------------------------------------------------------------------------------
| Node 3: yb-tserver (pid 72166)                                                                   |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL                : ./bin/ysqlsh -U postgres -h 127.0.0.3 -p 5433                              |
| YCQL                : ./bin/ycqlsh 127.0.0.3 9042                                                 |
| YEDIS               : ./bin/redis-cli -h 127.0.0.3 -p 6379                                       |
| data-dir[0]         : /Users/schoudhury/yugabyte-data/node-3/disk-1/yb-data                      |
| TServer Logs        : /Users/schoudhury/yugabyte-data/node-3/disk-1/yb-data/tserver/logs         |
----------------------------------------------------------------------------------------------------
```

## 2. Create a table

Create a YCQL table. Since you will be using a workload application in the [YugabyteDB workload generator](https://github.com/yugabyte/yb-sample-apps) to write data into this table, the keyspace and table name below must created exactly as shown.

```sh
$ ./bin/ycqlsh
```

```sql
ycqlsh> CREATE KEYSPACE ybdemo_keyspace;
```

```sql
ycqlsh> CREATE TABLE ybdemo_keyspace.cassandrakeyvalue (k text PRIMARY KEY, v blob);
```

For each table, you have instructed YugabyteDB to create four shards for each YB-TServer in this universe. Because you have three nodes, you expect 12 tablets for the `ybdemo_keyspace.cassandrakeyvalue` table.

## 3. Explore tablets

- The tablets are evenly balanced across the various nodes.

You can see the number of tablets per node in the Tablet Servers page of the master Admin UI, by going to the [table details page](http://127.0.0.1:7000/tablet-servers). The page should look something like the image below.

![Number of tablets in the table](/images/ce/auto-sharding-cassandra-table-1.png)

We see that each node has 4 tablets, and the total number of tablets is 12 as you expected.

- The table has 12 tablets, each owning a range of the keyspace.

Let us navigate to the [table details page](http://127.0.0.1:7000/table?keyspace_name=ybdemo_keyspace&table_name=cassandrakeyvalue) to examine the various tablets. This page should look as follows.

![Tablet details of the table](/images/ce/auto-sharding-cassandra-tablets.png)

What you see here is that there are 12 tablets as expected, and the key ranges owned by each tablet are shown. This page also shows which node that is currently hosting (and is the leader for) each of these tablets. Note here that the tablets balancing across nodes happens on a per-table basis, so that each table is scaled out to an appropriate number of nodes.

- Each tablet has a separate directory dedicated to it for data.

Let us list out all the tablet directories and see their sizes. This can be done as follows.

```sh
$ du -hs /yugabyte-data/node*/disk*/yb-data/tserver/data/rocksdb/table*/* | grep -v '0B'
```

```
 20K    /yugabyte-data/node-1/disk-1/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-439ae3bde90049d6812e198e76ad29a4
 20K    /yugabyte-data/node-1/disk-1/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-eecd01f0a7cd4537ba571bdb85d0c094
 20K    /yugabyte-data/node-1/disk-2/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-4ea334056a3845518cc6614baef96966
 20K    /yugabyte-data/node-1/disk-2/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-52642a3a9d7b4d38a103dff5dd77a3c6
 20K    /yugabyte-data/node-2/disk-1/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-4e31e26b3b204e34a1e0cfd6f7500525
 20K    /yugabyte-data/node-2/disk-1/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-b7ac08a983aa45a3843ab92b1719799a
 20K    /yugabyte-data/node-2/disk-2/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-22c349a07afb48e3844b570c24455431
 20K    /yugabyte-data/node-2/disk-2/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-8955db9e1ec841f3a30535b77d707586
 20K    /yugabyte-data/node-3/disk-1/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-adac9f92466b4d288a4ae346aaad3880
 20K    /yugabyte-data/node-3/disk-1/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-f04a6d5113a74ba79a04f01c80423ef5
 20K    /yugabyte-data/node-3/disk-2/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-1c472c1204fe40afbc7948dadce22be8
 20K    /yugabyte-data/node-3/disk-2/yb-data/tserver/data/rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-5aaeb96381044aa2b09ed9973830bb27
 ```

## 4. Insert and query a table

Let us insert a key-value entry, with the value size around 2MB. Since the memstores are configured to be 1MB, this will cause the data to flush to disk immediately. Note that the key flags you pass to the sample app are:

- `--num_unique_keys 1` - Write exactly one key. Keys are numbers converted to text, and typically start from 0.
- `--num_threads_read 0` - Do not perform any reads (hence 0 read threads).
- `--num_threads_write 1` - Since you are not writing a lot of data, a single writer thread is sufficient.
- `--value_size 10000000`  - Generate the value being written as a random byte string of around 10MB size.
- `--nouuid` - Do not prefix a UUID to the key. A UUID allows multiple instances of the load tester to run without interfering with each other.

Download the YugabyteDB workload generator JAR file (`yb-sample-apps.jar`) by running the following command.

```sh
$ wget https://github.com/yugabyte/yb-sample-apps/releases/download/1.3.1/yb-sample-apps.jar?raw=true -O yb-sample-apps.jar
```

Run the `CasandraKeyValue` workload application.

```sh
$ java -jar ./yb-sample-apps.jar --workload CassandraKeyValue \
                                    --nodes 127.0.0.1:9042 \
                                    --nouuid \
                                    --num_unique_keys 1 \
                                    --num_writes 2 \
                                    --num_threads_read 0 \
                                    --num_threads_write 1 \
                                    --value_size 10000000
```

```
2018-02-05 07:33:33,525 [INFO|...] Num unique keys to insert: 1
...
2018-02-05 07:33:36,899 [INFO|...] The sample app has finished
```

Let us check what we have inserted using `ycqlsh`.

```sh
$ ./bin/ycqlsh
```

```sql
ycqlsh> SELECT k FROM ybdemo_keyspace.cassandrakeyvalue;
```

```
 k
-------
 key:0

(1 rows)
```

Now let us check the sizes of the various tablets:

```sh
$ du -hs /yugabyte-data/node*/disk*/yb-data/tserver/data/rocksdb/table*/* | grep -v '0B'
```

```
 20K    .../rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-439ae3bde90049d6812e198e76ad29a4
9.6M    .../rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-eecd01f0a7cd4537ba571bdb85d0c094
 20K    .../rocksdb/table-9987797012ce4c1c91782c25e7608c34/tablet-4ea334056a3845518cc6614baef96966
 ...
 ```

We see that the key has been written to one of the tablets, in the case of this experiment the UUID of the tablet is `eecd01f0a7cd4537ba571bdb85d0c094`. We can find out from the [table details page](http://127.0.0.1:7000/table?keyspace_name=ybdemo_keyspace&table_name=cassandrakeyvalue) which node this tablet belongs to - it is `node-1` in this case. Here is the relevant screenshot.

![Tablet ownership with auto-sharding](/images/ce/auto-sharding-tablet-ownership.png)

We can also easily confirm that the `node-1` indeed has about 10MB of storage being used.

![Inserting values with auto-sharding](/images/ce/auto-sharding-single-kv-insert.png)

## 5. Automatic sharding when add nodes

Let us add one more node to the universe for a total of 4 nodes, by running the following command.

```sh
$ ./bin/yb-ctl add_node --tserver_flags "memstore_size_mb=1"
```

By looking at the tablet servers page, you find that the tablets are re-distributed evenly among the 4 nodes and each node now has 3 shards.

![Auto-sharding when adding one node](/images/ce/auto-sharding-add-1-node.png)

Next, let us add 2 more nodes to the universe, making it a total of 6 nodes. We can do this by running the following.

```sh
$ ./bin/yb-ctl add_node --tserver_flags "memstore_size_mb=1"
```

```sh
$ ./bin/yb-ctl add_node --tserver_flags "memstore_size_mb=1"
```

We can verify that the tablets are evenly distributed across the 6 nodes. Each node now has 2 tablets.

![Auto-sharding when adding three nodes](/images/ce/auto-sharding-add-3-node.png)

## 6. [Optional] Clean up

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./bin/yb-ctl destroy
```
