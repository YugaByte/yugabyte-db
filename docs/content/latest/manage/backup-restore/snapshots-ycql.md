---
title: Snapshot and restore data for YCQL
headerTitle: Snapshot and restore data for YCQL
linkTitle: Snapshot and restore data
description: Snapshot and restore data in YugabyteDB for YCQL.
image: /images/section_icons/manage/enterprise.png
aliases:
  - manage/backup-restore/manage-snapshots
menu:
  latest:
    identifier: snapshots-2-ycql
    parent: backup-restore
    weight: 705
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/manage/backup-restore/snapshot-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/manage/backup-restore/snapshots-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

You can create a transactional backup for a YCQL table (including associated secondary indexes) using snapshots.

## Step 1: Create a local cluster

To create a local cluster, see [Create a local cluster](../../../quick-start/create-local-cluster).

```sh
$ ./bin/yb-ctl create
```

```
Creating cluster.
Waiting for cluster to be ready.
----------------------------------------------------------------------------------------------------
| Node Count: 1 | Replication Factor: 1                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : postgresql://postgres@127.0.0.1:5433                                       |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /home/guru/yugabyte-data                                                   |
----------------------------------------------------------------------------------------------------

For more info, please use: yb-ctl status
```

For details on flags, see [yb-ctl reference](../../../admin/yb-ctl).

## Step 2: Create a table with data

After [getting started on YCQL API](../../../api/ycql/quick-start/), open `ycqlsh`:

```sh
$ ./bin/ycqlsh
```

Create a keyspace, table, index, and insert some test data.

```plpgsql
ycqlsh> CREATE KEYSPACE ydb;
ycqlsh> CREATE TABLE IF NOT EXISTS ydb.test_tb(user_id INT PRIMARY KEY, name TEXT) WITH transactions = {'enabled': true};
ycqlsh> CREATE INDEX test_tb_name ON ydb.test_tb(name);
ycqlsh> INSERT INTO ydb.test_tb(user_id,name) VALUES (5,'John Doe');
```

To verify that you have data in the database, run the following `SELECT` statement:

```plpgsql
ycqlsh> SELECT * FROM ydb.test_tb;

 user_id | name
---------+----------
       5 | John Doe

(1 rows)
```

## Step 3: Create a snapshot

Create a snapshot using the [`yb-admin create_snapshot`](../../../admin/yb-admin/#create-snapshot) command:

```sh
$ ./bin/yb-admin create_snapshot ydb test_tb
```

```
Started snapshot creation: a9442525-c7a2-42c8-8d2e-658060028f0e
```

To see when your snapshot is ready, run the [`yb-admin list_snapshots`](../../../admin/yb-admin/#list-snapshots) command.

```sh
./bin/yb-admin list_snapshots
```

```
Snapshot UUID                    	State
a9442525-c7a2-42c8-8d2e-658060028f0e 	COMPLETE
No snapshot restorations
```

### Step 3.1: Export the snapshot

Before exporting the snapshot, you need to export a metadata file that describes the snapshot.

```sh
$ ./bin/yb-admin export_snapshot a9442525-c7a2-42c8-8d2e-658060028f0e test_tb.snapshot
```

```
Exporting snapshot a9442525-c7a2-42c8-8d2e-658060028f0e (COMPLETE) to file test_tb.snapshot
Snapshot meta data was saved into file: test_tb.snapshot
```

Next, you need to copy the actual data from the table and tablets. In this case, you
have to use a script that copies all data. The file path structure is:

```sh
<yb_data_dir>/node-<node_number>/disk-<disk_number>/yb-data/tserver/data/rocksdb/table-<table_id>/[tablet-<tablet_id>.snapshots]/<snapshot_id>
```

- `<yb_data_dir>` is the directory where YugabyteDB data is stored. (default=`~/yugabyte-data`)
- `<node_number>` is used when multiple nodes are running on the same server (for testing, QA, and development). The default value is `1`.
- `<disk_number>` when running yugabyte on multiple disks with the `--fs_data_dirs` flag. The default value is `1`.
- `<table_id>` is the UUID of the table. You can get it from the `http://<yb-master-ip>:7000/tables` url in the Admin
 UI.
- `<tablet_id>` in each table there is a list of tablets. Each tablet has a `<tablet_id>.snapshots` directory that you need to copy.
- `<snapshot_id>` there is a directory for each snapshot since you can have multiple completed snapshots on each server.

This directory structure is specific to `yb-ctl`, which is a local testing tool.
In practice, for each server, you will use the `--fs_data_dirs` flag, which is a comma-separated list of paths where to put the data (normally different paths should be on different disks).
In this `yb-ctl` example, these are the full paths up to the `disk-x`.

### Step 3.2: Copy snapshot data to another directory

{{< note title="Tip" >}}

To get a snapshot of a multi-node cluster, you need to go into each node and copy
the folders of ONLY the leader tablets on that node. There is no need to keep a copy for each replica, since each tablet-replica has
a copy of the same data.

{{< /note >}}

First, get the `table_id` UUID that you want to snapshot. You can find the UUID in
the Admin UI (`http://127.0.0.1:7000/tables`) under **User Tables**.

For each table, there are multiple tablets where the data is stored. You need to get a list of tablets and the leader for each of them.

```sh
$ ./bin/yb-admin list_tablets ydb test_tb 0
```

```
$ ./bin/yb-admin list_tablets ydb test_tb 0
Tablet-UUID                      	Range                                                    	Leader-IP       	Leader-UUID
6a2bf658a3ea47f0ba2515ce484096ad 	partition_key_start: "" partition_key_end: "\177\377"    	127.0.0.1:9100  	8230396013f04c81bf86e684360cc87c
5a8eb39732904f769c57033e1301c84c 	partition_key_start: "\177\377" partition_key_end: ""    	127.0.0.1:9100  	8230396013f04c81bf86e684360cc87c
```

The third argument is for limiting the number of returned results. Setting it to `0` returns all tablets.
You need to do the same for the index `test_tb_name` that is linked to the table:

```sh
$ ./bin/yb-admin list_tablets ydb test_tb_name 0
```

```
Tablet-UUID                      	Range                                                    	Leader-IP       	Leader-UUID
fa9feea93b0b410388e9bf383f938039 	partition_key_start: "" partition_key_end: "\177\377"    	127.0.0.1:9100  	8230396013f04c81bf86e684360cc87c
1ac1047fb3354590968a6780fac89a67 	partition_key_start: "\177\377" partition_key_end: ""    	127.0.0.1:9100  	8230396013f04c81bf86e684360cc87c
```

Using this information, you can construct the full path of all directories where snapshots are stored for each
 (`table`, `tablet`, `snapshot_id`).

You can create a small script to manually copy, or move, the folders to a backup directory or external storage.

{{< note title="Tip" >}}

When doing RF1 as the source, the output of the `yb-admin`, like listing the tablets, only shows LEADERS because there's only one copy, which is the leader.

{{< /note >}}

## Step 4: Destroy the cluster and create a new one

Now destroy the cluster.

```sh
$ ./bin/yb-ctl destroy
```

```
Destroying cluster.
```

Next, spin up a new cluster with three nodes in the replicated setup.

```sh
./bin/yb-ctl --rf 3 create
```

```
Creating cluster.
Waiting for cluster to be ready.
----------------------------------------------------------------------------------------------------
| Node Count: 3 | Replication Factor: 3                                                            |
----------------------------------------------------------------------------------------------------
| JDBC                : postgresql://postgres@127.0.0.1:5433                                       |
| YSQL Shell          : bin/ysqlsh                                                                 |
| YCQL Shell          : bin/ycqlsh                                                                  |
| YEDIS Shell         : bin/redis-cli                                                              |
| Web UI              : http://127.0.0.1:7000/                                                     |
| Cluster Data        : /home/guru/yugabyte-data                                                   |
----------------------------------------------------------------------------------------------------

For more info, please use: yb-ctl status
```

{{< note title="Tip" >}}

Make sure to get the master IP address from [`yb-ctl status`](../../../admin/yb-ctl/#status) since you have multiple nodes on different IP addresses.

{{< /note >}}

## Step 5: Trigger snapshot import

{{< note title="Tip" >}}

The `keyspace` and `table` can be different from the exported one.

{{< /note >}}

First, import the snapshot file into YugabyteDB.

```sh
$ ./bin/yb-admin import_snapshot test_tb.snapshot
```

```
Read snapshot meta file test_tb.snapshot
Importing snapshot a9442525-c7a2-42c8-8d2e-658060028f0e (COMPLETE)
Table type: table
Table being imported: ydb.test_tb
Table type: index (attaching to the old table id cb612f9693fb40b6beeaa159078effd0)
Table being imported: ydb.test_tb_name
Successfully applied snapshot.
Object           	Old ID                           	New ID                          
Keyspace         	485a915f8f794308a6f39398040fada8 	6e407151f7ba41cf991f68dfdd5248b9
Table            	cb612f9693fb40b6beeaa159078effd0 	5550206e25d140698be031154a805823
Tablet 0         	6a2bf658a3ea47f0ba2515ce484096ad 	4da0ca52f96e4ed88f071196890550fd
Tablet 1         	5a8eb39732904f769c57033e1301c84c 	83b734a4e8d042a989a79a4340bc14e7
Keyspace         	485a915f8f794308a6f39398040fada8 	6e407151f7ba41cf991f68dfdd5248b9
Table            	6b538842e9f24f99b4b9ba2a995805fc 	2c59396c7e214a188dbdbcb3206b04d6
Tablet 0         	fa9feea93b0b410388e9bf383f938039 	456bf3575e6d41d2ba640386c1d9df26
Tablet 1         	1ac1047fb3354590968a6780fac89a67 	30fb27da04df46749129a42e9cf3289a
Waiting for table 2c59396c7e214a188dbdbcb3206b04d6...
Snapshot         	a9442525-c7a2-42c8-8d2e-658060028f0e 	27c331c0-4b5c-4027-85f9-75b7545641a7
```

After importing the `metadata file`, you see the following changes:

1. `Old ID` and `New ID` for table, tablets, and snapshot.
2. `table_id`, `tablet_id` and `snapshot_id` have changed, therefore the paths are different.
3. When restoring, you have to use the new IDs to get the right paths to move data.

Using these IDs, you can restore the previous `.snapshot` folders to the new paths.

{{< note title="Note" >}}

For each tablet, you need to copy the snapshots folder on all tablet peers and in any configured read replica cluster. 

{{< /note >}}

You can start restoring the snapshot using the [`yb-admin restore_snapshot`](../../../admin/yb-admin/#restore-snapshot) command:

```sh
$ ./bin/yb-admin restore_snapshot 27c331c0-4b5c-4027-85f9-75b7545641a7
```

```
Started restoring snapshot: 27c331c0-4b5c-4027-85f9-75b7545641a7
Restoration id: e982fe91-3b34-462a-971b-11d9e2ac1712
```

We get back a `Restoration id`, which we can check to see the status of the restore.

After some time, you can see that the restore has completed:

```
guru@guru-predator:~/Desktop/yugabyte/yugabyte-2.2.0.0$ ./bin/yb-admin list_snapshots
Snapshot UUID                    	State
27c331c0-4b5c-4027-85f9-75b7545641a7 	COMPLETE
Restoration UUID                 	State
e982fe91-3b34-462a-971b-11d9e2ac1712 	RESTORED
```

## Step 6: Verify the data

```sh
$ ./bin/ycqlsh
```

```plpgsql
ycqlsh> select * from ydb.test_tb;

 user_id | name
---------+----------
       5 | John Doe

(1 rows)
```

Finally, if no longer needed, you can delete the snapshot and increase disk space.

```sh
$ ./bin/yb-admin delete_snapshot 27c331c0-4b5c-4027-85f9-75b7545641a7
```

```
Deleted snapshot: 27c331c0-4b5c-4027-85f9-75b7545641a7
```

This was a guide on how to snapshot and restore data on YugabyteDB. In the Yugabyte Platform and Yugabyte Cloud, all of the manual steps above are automated.
