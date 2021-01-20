---
title: Back up data universe YSQL data
headerTitle: Back up universe YSQL data
linkTitle: Back up universe data
description: Use Yugabyte Platform to back up data in YSQL tables.
aliases:
  - /latest/manage/enterprise-edition/backup-restore
  - /latest/manage/enterprise-edition/back-up-restore-data
  - /latest/yugabyte-platform/manage/backup-restore-data
  - /latest/yugabyte-platform/back-up-restore-universes/back-up-universe-data/
menu:
  latest:
    parent: back-up-restore-universes
    identifier: back-up-universe-data-1-ysql
    weight: 20
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./ysql.md" >}}" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="{{< relref "./ycql.md" >}}" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

Use Yugabyte Platform to back up your YugabyteDB universe YSQL data. To schedule backups for a later time, or as a recurring task, see [Schedule universe YSQL data backups](../../schedule-data-backups/ysql).

To immediately back up your YugabyteDB universe YSQL data:

1. Open the **Universe Overview** and then click the **Backups** tab. The **Backups** page appears.
2. Click **Create Backup** to open the **Create Backup** dialog.

    ![Create Backup - YSQL](/images/yp/create-backup-ysql.png)

3. Click the **YSQL** tab and enter the following information:

    - **Storage**: Select the storage type: `GCS Storage`, `S3 Storage`, or `NFS Storage`.
    - **Namespace**: Select the namespace from the drop-down list of available namespaces.
    - **Parallel Threads**: Enter or select the number of threads. The default value of `8` appears.

Click **OK**. The requested backup begins immediately.

{{< note title="Note" >}}

If the universe has encrypted at rest enabled, data files are backed up as-is (encrypted) to reduce the computation cost of a backup and to keep the files encrypted.
A universe key metadata file, containing key references, is also backed up.

{{< /note >}}
