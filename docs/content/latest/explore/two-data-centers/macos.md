---
title: Explore two data center (2DC) deployment on macOS
headerTitle: Two data center (2DC) deployment
linkTitle: Two data center (2DC)
description: Simulate a geo-distributed two data center (2DC) deployment with two local YugabyteDB clusters on macOS.
aliases:
  - /latest/explore/two-data-centers/
  - /latest/explore/two-data-centers-macos/
menu:
  latest:
    identifier: two-data-centers-1-macos
    parent: explore
    weight: 250
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/two-data-centers/macos" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/two-data-centers/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

</ul>

By default, YugabyteDB provides synchronous replication and strong consistency across geo-distributed data centers. But sometimes asynchronous replication will meet your need for disaster recovery, auditing and compliance, and other applications. For more information, see [Two data center (2DC) deployments](../../../architecture/2dc-deployments/) in the Architecture section.

This tutorial simulates a geo-distributed two data center (2DC) deployment using two local YugabyteDB clusters, one representing "Data Center - East" and the other representing "Data Center - West." You can explore unidirectional (master-follower) asynchronous replication and bidirectional (multi-master) asynchronous replication using the [yb-ctl](../../../admin/yb-ctl) and [yb-admin](../../../admin/yb-admin) utilities.

## Prerequisites

- Verify that you have the required extra loopback addresses by reviewing the [Quick Start section](../../../quick-start/install/macos/#configure).

- For the tutorial, use the default database `yugabyte` and the default user `yugabyte`.

## 1. Create two "data centers"

Create and start your first local cluster that will simulate "Data Center - East" by running the following `yb-ctl create` command from your YugabyteDB home directory.

```sh
$ ./bin/yb-ctl create --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-east --ip_start 1
```

This will start up a one-node local cluster using the IP address of `127.0.0.1:7100` and create `yb-datacenter-east` as the data directory. Upon starting, you should see a screen like the following.

```
Creating cluster.
Waiting for cluster to be ready.
----------------------------------------------------------------------------------------------------
| Node Count: 1 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.1:5433/postgres                                  |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                 |
| YEDIS Shell         : bin/redis-cli                                                              |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /Users/yugabyte_user/yugabyte/yb-datacenter-east                           |
----------------------------------------------------------------------------------------------------
```

Create and start your second local cluster that will simulate "Data Center = West" by running the following `yb-ctl create` command from your YugabyteDB home directory.

```sh
$ ./bin/yb-ctl create --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-west --ip_start 2
```

This will start up a one-node cluster using IP address of `127.0.0.2` and create `yb-datacenter-west` as the data directory. Upon starting, you should see a screen like the following.

```
Creating cluster.
Waiting for cluster to be ready.
----------------------------------------------------------------------------------------------------
| Node Count: 1 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : jdbc:postgresql://127.0.0.2:5433/postgres                                  |
| YSQL Shell          : bin/ysqlsh -h 127.0.0.2                                                    |
| YCQL Shell          : bin/ycqlsh 127.0.0.2                                                        |
| YEDIS Shell         : bin/redis-cli -h 127.0.0.2                                                 |
| Web UI              : http://127.0.0.2:7000/                                                     |
| Cluster Data        : /Users/yugabyte_user/yugabyte/yb-datacenter-west                           |
----------------------------------------------------------------------------------------------------
```

## 2. Create database tables

In the default `yugabyte` database, create the database table `users` on the "Data Center - East" cluster.

Open `ysqlsh` specifying the host IP address of `127.0.0.1`.

```sh
$ ./bin/ysqlsh -h 127.0.0.1
```

Run the following `CREATE TABLE` statement.

```postgresql
CREATE TABLE users (
    email varchar(35) PRIMARY KEY,
    username varchar(20)
    );
```

Now create the identical database table on cluster B.

Open `ysqlsh` for "Data Center - West" by specifying the host IP address of `127.0.0.2`.

```sh
$ ./bin/ysqlsh -h 127.0.0.2
```

Run the following `CREATE TABLE` statement.

```postgresql
CREATE TABLE users (
    email varchar(35) PRIMARY KEY,
    username varchar(20)
    );
```

You now have the identical database table on each of your clusters and can now set up 2DC asynchronous replication.

## 3. Configure unidirectional replication

To configure "Data Center - West" to be the consumer of data changes from the "Data Center - East" cluster, you need to use the `yb-admin` `setup_universe_replication` command. Review the syntax and then you can run the command.

```sh
yb-admin -master_addresses <consumer-master-addresses> \ 
setup_universe_replication <producer-universe_uuid> <producer_master_addresses> <producer-table-ids>
```

- *consumer-master-addresses*: a comma-separated list of the YB-Master servers. For this simulation, you have one YB-Master server for each cluster (typically, there are three).
- *producer-universe-uuid*: a unique identifier for the producer cluster. The UUID can be found in the YB-Master UI (`<yb-master-ip>:7000`).
- *producer-table-ids*: A comma-separated list of `table_id` values. The generated UUIDs can be found in the YB-Master UI (`<yb-master-ip>:7000`/tables).

Based on your actual values (which you got from the YB-Master UI page at `yb-master-ip>:7000`), run the `yb-admin` `setup_universe_replication` command like in this example.

- consumer-master-addresses: `127.0.0.2:7100`
- producer-universe-uuid: `7acd6399-657d-42dc-a90a-646869898c2d`
- producer-master-addresses: `127.0.0.1:7100`
- producer-table-ids: `000030a9000030008000000000004000`

```sh
$ ./bin/yb-admin -master_addresses 127.0.0.2:7100 \
setup_universe_replication 7acd6399-657d-42dc-a90a-646869898c2d 127.0.0.1:7100 000030a9000030008000000000004000
```

You should see a message like the following:

```
Replication setup successfully
```

## 4. Verify unidirectional replication

Now that you've configured unidirectional replication, you can now add data to the `users` table on the "Data Center - East" cluster and see the data appear in the `users` table on "Data Center - West" cluster.

To add data to the "Data Center - East" cluster, open `ysqlsh` by running the following command, making sure you are pointing to the new producer host.

```sh
$ ./bin/ysqlsh -host 127.0.0.1
```

```postgresql
yugabyte=# INSERT INTO users(email, username) VALUES ('hector@example.com', 'hector'), ('steve@example.com', 'steve');
```

On the consumer "Data Center - West" cluster, open `ysqlsh` and run the following to quickly see that data has been replicated between clusters.

```sh
$ ./bin/ysqlsh -host 127.0.0.2
```

```postgresql
yugabyte=# SELECT * FROM users;
```

You should see the following in the results.

```
       email         | username
---------------------+----------
 hector@example.com  | hector
 steve@example.com   | steve
(2 rows)
```

## 5. Configure bidirectional replication (optional)

Bidirectional asynchronous replication lets you insert data into the same table on either of the clusters and have the data changes added to the other cluster.

To configure bidirectional asynchronous replication for the same table, you need to run the following `yb-admin` `setup_universe_replication` command to set up the "Data Center - East" cluster to be the consumer of the "Data Center - West" cluster. For this example, here are the values used and the command example.

- consumer-master-addresses: `127.0.0.1:7100`
- producer-universe-uuid: `0a315687-e9bd-430f-b6f4-ac831193a394`
- producer-master-addresses: `127.0.0.2:7100`
- producer-table-ids: `000030a9000030008000000000004000`

```sh
$ ./bin/yb-admin -master_addresses 127.0.0.1:7100 \
setup_universe_replication 0a315687-e9bd-430f-b6f4-ac831193a394  127.0.0.2:7100 000030a9000030008000000000004000
```

You should see a message that shows the following:

```
Replication setup successfully
```

## 6. Verify bidirectional replication (optional)

Now that you've configured bidirectional replication, you can now add data to the `users` table on the "Data Center - West" cluster and see the data appear in the `users` table on "Data Center - East" cluster.

To add data to the "Data Center - West" cluster, open `ysqlsh` by running the following command, making sure you are pointing to the new producer host.

```sh
$ ./bin/ysqlsh -host 127.0.0.2
```

```postgresql
yugabyte=# INSERT INTO users(email, username) VALUES ('neha@example.com', 'neha'), ('mikhail@example.com', 'mikhail');
```

On the new "consumer" cluster, open `ysqlsh` and run the following to quickly see that data has been replicated between clusters.

```sh
$ ./bin/ysqlsh -host 127.0.0.1
```

```postgresql
yugabyte=# SELECT * FROM users;
```

You should see the following in the results.

```
       email         | username
---------------------+----------
 hector@example.com  | hector
 steve@example.com   | steve
 neha@example.com    | neha
 mikhail@example.com | mikhail
(4 rows)
```

## 7. Clean up

At this point, you've finished the tutorial. You can either stop and save your examples or destroy and remove the clusters and their associated data directories.

To stop the simulated "data centers", use the `yb-ctl stop` commands using the `--data_dir` option to specify the cluster.

**Example - stopping "Data Center - East"** 

```sh
$ ./bin/yb-ctl stop --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-east
```

To destroy the simulated "data centers" and remove its associate data directory, use the `yb-ctl destroy` command with the `--data_dir` option to specify the cluster.

**Example — destroying and removing the "Data Center - West"**

```sh
$ ./bin/yb-ctl destroy --data_dir /Users/yugabyte_user/yugabyte/yb-datacenter-west
```

## What's next?

For more information, see the following in the Architecture section:

- [Two data center (2DC) deployments](../../../architecture/2dc-deployments/)
- [Change data capture (CDC)](../../../architecture/cdc-architecture)
