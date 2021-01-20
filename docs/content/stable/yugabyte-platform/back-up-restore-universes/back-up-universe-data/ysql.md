---
title: Back up data universe YSQL data
headerTitle: Back up universe YSQL data
linkTitle: Back up universe data
description: Use Yugabyte Platform to back up data in YSQL tables.
aliases:
  - /stable/manage/enterprise-edition/backup-restore
  - /stable/manage/enterprise-edition/back-up-restore-data
  - /stable/yugabyte-platform/manage/backup-restore-data
  - /stable/yugabyte-platform/back-up-restore-universes/back-up-universe-data/
menu:
  stable:
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

To immediately back up your YugabyteDB universe YSQL data, follow these steps.

1. Open the **Universe Overview** and then click the **Backups** tab. The **Backups** page appears.
2. Click **Create Backup** to open the **Create Backup** dialog.

    ![Create Backup - YSQL](/images/yp/create-backup-ysql.png)

3. Click the **YSQL** tab and enter the following:

    - **Storage**: Select the storage type: `GCS Storage`, `S3 Storage`, or `NFS Storage`.
    - **Namespace**: Select the namespace from the drop-down list of available namespaces.
    - **Parallel Threads**: Enter or select the number of threads. The default value of `8` appears.

Click **OK**. The backup begins immediately.
