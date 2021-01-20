---
title: Restore universe YSQL data
headerTitle: Restore universe YSQL data
linkTitle: Restore universe data
description: Use Yugabyte Platform to restore data in YSQL tables.
aliases:
- /stable/yugabyte-platform/back-up-restore-databases/
- /stable/yugabyte-platform/back-up-restore-universes/restore-universe-data/
menu:
  stable:
    parent: back-up-restore-universes
    identifier: restore-universe-data-1-ysql
    weight: 30
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./ysql.md" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="{{< relref "./ycql.md" >}}" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

On that same completed task, click on the **Actions** dropdown and click **Restore Backup**.
You will see a modal where you can select the universe, keyspace, and table you want to restore to. Enter in
values like this (making sure to change the table name you restore to) and click **OK**.

To restore YugabyteDB universe YCQL data from a backup, follow these steps.

1. Open the **Universe Overview** and then click the **Backups** tab. The **Backups** page appears.
2. Click **Restore Backup** to open the **Restore data to** dialog.

    ![Restore backup - YCQL](/images/yp/restore-backup-ycql.png)

3. Complete the following fields:

    - **Storage** Select the storage configuration type: `GCS Storage', 'S3 Storage', or 'NFS Storage'.
    - **Storage Location**: Specify the storage location.
    - **Universe**: Select the universe to restore.
    - **Keyspace**: Leave blank. This is used for YCQL data only.
    - **Table**: Leave blank. Backup and restore of YSQL data is for full universe data only.
    - **Parallel Threads**: Default is `8`. This value can be change to a value between `1` and `100`.
    - **KMS Configuration**: (optional) If the backup was from a universe that was encrypted at rest, then select the KMS     configuration to use.

4. Click **OK**. The restore begins immediately. When the restore is completed, a completed **Restore Backup** task will appear in the **Tasks** tab.
5. To confirm the restore succeeded, go to the **Tables** tab to compare the original table with the table you
restored to.

![Tables View](/images/yp/tables-view.png)