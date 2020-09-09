---
title: Snapshot and restore data for YCQL
headerTitle: Snapshot and restore data
linkTitle: Snapshot and restore data
description: Snapshot and restore data in YugabyteDB for YCQL.
image: /images/section_icons/manage/enterprise.png
block_indexing: true
menu:
  v2.1:
    identifier: snapshots-ycql
    parent: backup-restore
    weight: 705
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/manage/backup-restore/snapshots" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

You can create a backup for YugabyteDB using snapshots. Here are some points to keep in mind.

- Distributed backups using snapshots
  - Massively parallel, efficient for very large data sets
  - Snapshots are not transactional across the whole table, but only on each tablet [#2086](https://github.com/YugaByte/yugabyte-db/issues/2086).
  - Multi-table transactional snapshot is in the road map [#2084](https://github.com/YugaByte/yugabyte-db/issues/2084). 
  - Single table snapshots don't work in YSQL [#2083](https://github.com/YugaByte/yugabyte-db/issues/2083).
  - Yugabyte Platform automates these steps for you.
- Implementation notes:
  - Once the snapshot command is issued, you will “buffer” newly incoming writes to that tablet without writing them immediately.
  - For the existing data: you flush it to disk and hardlink the files in a `.snapshots` directory on each tablet.
  - These steps are pretty fast - small flush to disk and hardlinks. Most likely the incoming operations that were buffered will not timeout. 
  - The buffered writes are now opened up for writes.
  - The snapshot operation is done. Because YugabyteDB is an LSM database, these files will never get modified.
  - If this takes longer, some ops can timeout but in practice, users should expect such slowness occasionally when using network storage (AWS EBS, Persistent Disk in GCP, SAN storage, etc.).

In this tutorial, you will be using YCQL, but the same APIs are used in YSQL.

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

```
$ ./bin/ycqlsh
```

Create a keyspace, table, and insert some test data.

```
ycqlsh> CREATE KEYSPACE ydb;
ycqlsh> CREATE TABLE IF NOT EXISTS ydb.test_tb(user_id INT PRIMARY KEY);
ycqlsh> INSERT INTO ydb.test_tb(user_id) VALUES (5);
```

You can verify that you have data in the database by running a simple SELECT statement.

```
ycqlsh> SELECT * FROM ydb.test_tb;
```

```
 user_id
---------
       5

(1 rows)

```

## Step 3: Create a snapshot

Create a snapshot using the [`yb-admin create_snapshot`](../../../admin/yb-admin/#create-snapshot) command:

```sh
$ ./bin/yb-admin create_snapshot ydb test_tb
Started flushing table ydb.test_tb
Flush request id: fe0db953a7a5416c90f01b1e11a36d24
Waiting for flushing...
Flushing complete: SUCCESS
Started snapshot creation: 4963ed18fc1e4f1ba38c8fcf4058b295
```

To see when your snapshot is ready, you can run the [`yb-admin list_snapshots`](../../../admin/yb-admin/#list-snapshots) command.

```sh
$ ./bin/yb-admin list_snapshots
Snapshot UUID                    	State
4963ed18fc1e4f1ba38c8fcf4058b295 	COMPLETE
```

### Step 3.1: Export the snapshot

Before exporting the snapshot, you need to export a metadata file that describes the snapshot.

```sh
$ ./bin/yb-admin export_snapshot 4963ed18fc1e4f1ba38c8fcf4058b295 test_tb.snapshot
```

```
Exporting snapshot 4963ed18fc1e4f1ba38c8fcf4058b295 (COMPLETE) to file test_tb.snapshot
Snapshot meta data was saved into file: test_tb.snapshot
```

Next, you need to copy the actual data from the table and tablets. In this case, you
have to use a script that copies all data. The file path structure is:

```
<yb_data_dir>/node-<node_number>/disk-<disk_number>/yb-data/tserver/data/rocksdb/table-<table_id>/[tablet-<tablet_id>.snapshots]/<snapshot_id>
```

- `<yb_data_dir>` is the directory where YugabyteDB data is stored. (default=`~/yugabyte-data`)
- `<node_number>` is used when multiple nodes are running on the same server (for testing, QA, and development). The default value is `1`.
- `<disk_number>` when running yugabyte on multiple disks with the `--fs_data_dirs` flag. The default value is `1`.
- `<table_id>` is the UUID of the table. You can get it from the Admin UI.
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
Tablet UUID                      	Range                                                    	Leader
cea3aaac2f10460a880b0b4a2a4b652a 	partition_key_start: "" partition_key_end: "\177\377"    	127.0.0.1:9100
e509cf8eedba410ba3b60c7e9138d479 	partition_key_start: "\177\377" partition_key_end: ""    	127.0.0.1:9100
```

The third argument is for limiting the number of returned results. Setting it to `0` returns all tablets.

Using this information, you can construct the full path of all directories where snapshots are stored for each (`tablet`, `snapshot_id`).

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
$ ./bin/yb-admin import_snapshot test_tb.snapshot ydb test_tb
```

```
Read snapshot meta file test_tb.snapshot
Importing snapshot 4963ed18fc1e4f1ba38c8fcf4058b295 (COMPLETE)
Target imported table name: ydb.test_tb
Table being imported: ydb.test_tb
Successfully applied snapshot.
Object           	Old ID                           	New ID                          
Keyspace         	c478ed4f570841489dd973aacf0b3799 	c478ed4f570841489dd973aacf0b3799
Table            	ff4389ee7a9d47ff897d3cec2f18f720 	ff4389ee7a9d47ff897d3cec2f18f720
Tablet 0         	cea3aaac2f10460a880b0b4a2a4b652a 	cea3aaac2f10460a880b0b4a2a4b652a
Tablet 1         	e509cf8eedba410ba3b60c7e9138d479 	e509cf8eedba410ba3b60c7e9138d479
Snapshot         	4963ed18fc1e4f1ba38c8fcf4058b295 	4963ed18fc1e4f1ba38c8fcf4058b295
```

After you import the `metadata file`, you see some changes:

1. `Old ID` and `New ID` for table and tablets.
2. `table_id` and `tablet_id` have changed, so you have a different paths from previously.
3. Each `tablet_id` has changed, so you have different `tablet-<tablet_id>` directories.
4. When restoring, you have to use the new IDs to get the right paths to move data.

Using these IDs, you can restore the previous `.snapshot` folders to the new paths.

{{< note title="Tip" >}}

For each tablet, you need to copy the snapshots folder on all replicas.

{{< /note >}}

You can start restoring the snapshot using the [`yb-admin restore_snapshot`](../../../admin/yb-admin/#restore-snapshot) command:

```sh
$ ./bin/yb-admin restore_snapshot 4963ed18fc1e4f1ba38c8fcf4058b295
```

```
Started restoring snapshot: 4963ed18fc1e4f1ba38c8fcf4058b295
```

After some time, you can see that the restore has completed:

```
$ ./bin/yb-admin list_snapshots
Snapshot UUID                    	State
4963ed18fc1e4f1ba38c8fcf4058b295 	COMPLETE
```

## Step 6: Verify the data

```sh
$ ./bin/ycqlsh
```

```
ycqlsh> SELECT * FROM ydb.test_tb;

 user_id
---------
       5

```

Finally, if no longer needed, you can delete the snapshot and increase disk space.

```sh
$ ./bin/yb-admin delete_snapshot 4963ed18fc1e4f1ba38c8fcf4058b295
```

```
Deleted snapshot: 4963ed18fc1e4f1ba38c8fcf4058b295
```

This was a guide on how to snapshot and restore data on YugabyteDB. In the Yugabyte Platform and Yugabyte Cloud, all of the manual steps above are automated.