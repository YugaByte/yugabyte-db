---
title: Snapshot and restore data for YCQL
headerTitle: Snapshot and restore data for YCQL
linkTitle: Snapshot and restore data
description: Snapshot and restore data in YugabyteDB for YCQL.
image: /images/section_icons/manage/enterprise.png
menu:
  stable:
    identifier: snapshots-2-ycql
    parent: backup-restore
    weight: 705
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/manage/backup-restore/snapshot-ysql" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/stable/manage/backup-restore/snapshots-ycql" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

You can create a transactional backup for a YCQL table (including associated secondary indexes) using snapshots.

**Implementation notes**:

* Massively parallel, efficient for very large data sets.
* Once the snapshot command is issued, the database will “buffer” newly incoming writes to that tablet without writing them immediately.
* The existing data will be flushed to disk and hard links to the files will be created in a `.snapshots` directory on each tablet.
* The flush to disk and creation of hard links happen quickly. In most cases, the buffered incoming operations won't time out. 
* The snapshot operation is done. Because YugabyteDB is an LSM database, these files will never get modified.
* If the snapshot takes an unusually long time, some operations may time out. In practice, users should expect such slowness occasionally when using network storage (such as AWS EBS, Persistent Disk in GCP, or SAN storage).

## Try it out

To demonstrate YugabyteDB's snapshot functionality, the following example steps through creating a local cluster, adding a table, creating a snapshot, and then restoring that snapshot onto a fresh cluster.

{{< tip title="Automation" >}}

This guide explains how to snapshot and restore data on YugabyteDB. Yugabyte Platform and Yugabyte Cloud automate all of these manual steps.

{{< /tip >}}

### Create a snapshot

1. Create a new cluster.

    For more information on creating a local cluster, refer to [Create a local cluster](../../../quick-start/create-local-cluster). For details on flags, refer to the [yb-ctl reference](../../../admin/yb-ctl).

    ```sh
    $ ./bin/yb-ctl create
    ```

    ```output
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

1. Open `ycqlsh`, the YCQL shell.

    ```sh
    $ ./bin/ycqlsh
    ```

1. Create a keyspace, table, index, and insert some test data.

    ```sql
    ycqlsh> CREATE KEYSPACE ydb;
    ycqlsh> CREATE TABLE IF NOT EXISTS ydb.test_tb(user_id INT PRIMARY KEY, name TEXT) WITH transactions = {'enabled': true};
    ycqlsh> CREATE INDEX test_tb_name ON ydb.test_tb(name);
    ycqlsh> INSERT INTO ydb.test_tb(user_id,name) VALUES (5,'John Doe');
    ```

1. Run the following `SELECT` statement to verify that you have data in the database:

    ```sql
    ycqlsh> SELECT * FROM ydb.test_tb;
    ```

    ```output
    user_id | name
    ---------+----------
          5 | John Doe

    (1 rows)
    ```

1. Create a snapshot using the [`yb-admin create_snapshot`](../../../admin/yb-admin/#create-snapshot) command:

    ```sh
    $ ./bin/yb-admin create_snapshot ydb test_tb
    ```

    ```output
    Started snapshot creation: a9442525-c7a2-42c8-8d2e-658060028f0e
    ```

1. To see when your snapshot is ready, run the [`yb-admin list_snapshots`](../../../admin/yb-admin/#list-snapshots) command.

    ```sh
    ./bin/yb-admin list_snapshots
    ```

    ```output
    Snapshot UUID                         State
    a9442525-c7a2-42c8-8d2e-658060028f0e  COMPLETE
    No snapshot restorations
    ```

### Export the snapshot

1. Before you export the snapshot, export a metadata file that describes the snapshot.

    ```sh
    $ ./bin/yb-admin export_snapshot a9442525-c7a2-42c8-8d2e-658060028f0e test_tb.snapshot
    ```

    ```output
    Exporting snapshot a9442525-c7a2-42c8-8d2e-658060028f0e (COMPLETE) to file test_tb.snapshot
    Snapshot meta data was saved into file: test_tb.snapshot
    ```

1. Copy the actual data from the table and tablets. In this case, you have to use a script that copies all data. The file path structure is:

    ```sh
    <yb_data_dir>/node-<node_number>/disk-<disk_number>/yb-data/tserver/data/rocksdb/table-<table_id>/[tablet-<tablet_id>.snapshots]/<snapshot_id>
    ```

    * `<yb_data_dir>` is the directory where YugabyteDB data is stored. (default=`~/yugabyte-data`)
    * `<node_number>` is used when multiple nodes are running on the same server (for testing, QA, and development). The default value is `1`.
    * `<disk_number>` when running yugabyte on multiple disks with the `--fs_data_dirs` flag. The default value is `1`.
    * `<table_id>` is the UUID of the table. You can get it from the `http://<yb-master-ip>:7000/tables` url in the Admin UI.
    * `<tablet_id>` in each table there is a list of tablets. Each tablet has a `<tablet_id>.snapshots` directory that you need to copy.
    * `<snapshot_id>` there is a directory for each snapshot since you can have multiple completed snapshots on each server.

    This directory structure is specific to `yb-ctl`, which is a local testing tool.
    In practice, for each server, you will use the `--fs_data_dirs` flag, which is a comma-separated list of paths where to put the data (normally different paths should be on different disks).
    In this `yb-ctl` example, these are the full paths up to the `disk-x`.

### Copy snapshot data

{{< tip title="Tip" >}}

To get a snapshot of a multi-node cluster, you need to go into each node and copy
the folders of ONLY the leader tablets on that node. There is no need to keep a copy for each replica, since each tablet-replica has
a copy of the same data.

{{< /tip >}}

1. Get the `table_id` UUID that you want to snapshot. You can find the UUID in the Admin UI (`http://127.0.0.1:7000/tables`) under **User Tables**.

1. For each table, there are multiple tablets where the data is stored. Get a list of tablets and the leader for each of them.

    ```sh
    $ ./bin/yb-admin list_tablets ydb test_tb 0
    ```

    ```output
    Tablet-UUID                       Range                                                  Leader-IP       Leader-UUID
    6a2bf658a3ea47f0ba2515ce484096ad  partition_key_start: "" partition_key_end: "\177\377"  127.0.0.1:9100  8230396013f04c81bf86e684360cc87c
    5a8eb39732904f769c57033e1301c84c  partition_key_start: "\177\377" partition_key_end: ""  127.0.0.1:9100  8230396013f04c81bf86e684360cc87c
    ```

    The third argument is for limiting the number of returned results. Setting it to `0` returns all tablets.

1. Get the same list for the index `test_tb_name` that is linked to the table:

    ```sh
    $ ./bin/yb-admin list_tablets ydb test_tb_name 0
    ```

    ```output
    Tablet-UUID                       Range                                                  Leader-IP       Leader-UUID
    fa9feea93b0b410388e9bf383f938039  partition_key_start: "" partition_key_end: "\177\377"  127.0.0.1:9100  8230396013f04c81bf86e684360cc87c
    1ac1047fb3354590968a6780fac89a67  partition_key_start: "\177\377" partition_key_end: ""  127.0.0.1:9100  8230396013f04c81bf86e684360cc87c
    ```

1. Using this information, you can construct the full path of all directories where snapshots are stored for each (`table`, `tablet`, `snapshot_id`).

    You can create a small script to manually copy, or move, the folders to a backup directory or external storage.

    {{< tip title="Tip" >}}

When doing RF1 as the source, the output of `yb-admin`, like listing the tablets, only shows LEADERS because there's only one copy, which is the leader.

    {{< /tip >}}

### Destroy and re-create the cluster

1. Destroy the cluster.

    ```sh
    $ ./bin/yb-ctl destroy
    ```

    ```output
    Destroying cluster.
    ```

1. Spin up a new cluster with three nodes in the replicated setup.

    ```sh
    ./bin/yb-ctl --rf 3 create
    ```

    ```output
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

{{< tip title="Tip" >}}

Make sure to get the master IP address from [`yb-ctl status`](../../../admin/yb-ctl/#status) since you have multiple nodes on different IP addresses.

{{< /tip >}}

### Trigger snapshot import

{{< tip title="Tip" >}}

The `keyspace` and `table` can be different from the exported one.

{{< /tip >}}

1. Import the snapshot file into YugabyteDB.

    ```sh
    $ ./bin/yb-admin import_snapshot test_tb.snapshot
    ```

    ```output
    Read snapshot meta file test_tb.snapshot
    Importing snapshot a9442525-c7a2-42c8-8d2e-658060028f0e (COMPLETE)
    Table type: table
    Table being imported: ydb.test_tb
    Table type: index (attaching to the old table id cb612f9693fb40b6beeaa159078effd0)
    Table being imported: ydb.test_tb_name
    Successfully applied snapshot.
    Object            Old ID                            New ID                          
    Keyspace          485a915f8f794308a6f39398040fada8  6e407151f7ba41cf991f68dfdd5248b9
    Table             cb612f9693fb40b6beeaa159078effd0  5550206e25d140698be031154a805823
    Tablet 0          6a2bf658a3ea47f0ba2515ce484096ad  4da0ca52f96e4ed88f071196890550fd
    Tablet 1          5a8eb39732904f769c57033e1301c84c  83b734a4e8d042a989a79a4340bc14e7
    Keyspace          485a915f8f794308a6f39398040fada8  6e407151f7ba41cf991f68dfdd5248b9
    Table             6b538842e9f24f99b4b9ba2a995805fc  2c59396c7e214a188dbdbcb3206b04d6
    Tablet 0          fa9feea93b0b410388e9bf383f938039  456bf3575e6d41d2ba640386c1d9df26
    Tablet 1          1ac1047fb3354590968a6780fac89a67  30fb27da04df46749129a42e9cf3289a
    Waiting for table 2c59396c7e214a188dbdbcb3206b04d6...
    Snapshot          a9442525-c7a2-42c8-8d2e-658060028f0e  27c331c0-4b5c-4027-85f9-75b7545641a7
    ```

1. After importing the metadata file, note the following changes:

    * `Old ID` and `New ID` for table, tablets, and snapshot.
    * `table_id`, `tablet_id` and `snapshot_id` have changed, therefore the paths are different.

    When restoring, you have to use the new IDs to get the right paths to move data. Using these IDs, you can restore the previous `.snapshot` folders to the new paths.

    {{< note title="Note" >}}

For each tablet, you need to copy the snapshots folder on all tablet peers and in any configured read replica cluster. 

    {{< /note >}}

1. Start restoring the snapshot using the [`yb-admin restore_snapshot`](../../../admin/yb-admin/#restore-snapshot) command:

    ```sh
    $ ./bin/yb-admin restore_snapshot 27c331c0-4b5c-4027-85f9-75b7545641a7
    ```

    ```output
    Started restoring snapshot: 27c331c0-4b5c-4027-85f9-75b7545641a7
    Restoration id: e982fe91-3b34-462a-971b-11d9e2ac1712
    ```

1. Use the `Restoration id` from the previous step to check the status of the restore. It may take some time for the restore to be completed.

    ```sh
    $ ./bin/yb-admin list_snapshots
    ```

    ```output
    Snapshot UUID                         State
    27c331c0-4b5c-4027-85f9-75b7545641a7  COMPLETE
    Restoration UUID                      State
    e982fe91-3b34-462a-971b-11d9e2ac1712  RESTORED
    ```

### Verify the restored data

1. Verify that the import succeeded:

    ```sh
    $ ./bin/ycqlsh
    ```

    ```sql
    ycqlsh> select * from ydb.test_tb;
    ```

    ```output
     user_id | name
    ---------+----------
           5 | John Doe
    (1 rows)
    ```

1. If no longer needed, delete the snapshot and reclaim the disk space it was using.

    ```sh
    $ ./bin/yb-admin delete_snapshot 27c331c0-4b5c-4027-85f9-75b7545641a7
    ```

    ```output
    Deleted snapshot: 27c331c0-4b5c-4027-85f9-75b7545641a7
    ```
