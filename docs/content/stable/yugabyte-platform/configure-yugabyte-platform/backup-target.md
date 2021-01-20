---
title: Configure the backup target
headerTitle: Configure the backup target
linkTitle: Configure the backup target
description: Configure the backup target.
menu:
  stable:
    identifier: backup-target
    parent: configure-yugabyte-platform
    weight: 30
isTocNested: true
showAsideToc: true
---

Configure the backup target for YugabyteDB universe data by following the steps below for your backup storage option.

### Local storage

To create a local directory on a tserver to back up to, follow these steps:

1. Select **Connect** in the **Nodes** tab of the universe and then select the server from **Admin Host**.

    ![Connect Modal](/images/ee/br-connect-modal.png)

2. While connected using `ssh`, create a directory `/backup` and then change the owner to `yugabyte`.

    ```sh
    $ sudo mkdir /backup; sudo chown yugabyte /backup
    ```

{{< note title="Note" >}}

When you have more than one node, an `nfs` mounted on each server is recommended, and
creating a local backup folder on each server will not work.

{{< /note >}}

## Amazon S3

To configure Amazon S3 as the backup target, follow these steps:

1. Click **Configs** on the left panel.
2. Select the **Backup** tab.
3. Click **Amazon S3** and enter values for **Access Key**, **Access Secret**, **S3 Bucket**, and **S3 Bucket Host Base**.

4. Click **Save**.

![AWS Backup](/images/ee/cloud-provider-configuration-backup-aws.png)

The **Access Key** and **Access Secret** values can be added for the IAM of the user.

### NFS

To configure NFS as the backup target, follow these steps:

1. Select **Configuration** on the left panel.
2. Select the **Backup** tab.
3. Click **NFS** and enter the **NFS Storage Path** (`/backup` or another directory).
4. Click **Save**.

![NFS Cloud Provider Configuration](/images/ee/cloud-provider-configuration-backup-nfs.png)

### Google Cloud Storage (GCS)

To configure NFS as the backup target, follow these steps:

1. Click **Configs** on the left panel.
2. Click the **Backup** tab.
3. Click **GCS** and enter values for **GCS Bucket** and **GCS Credentials**.
4. Click **Save**.

![GCS Backup](/images/ee/cloud-provider-configuration-backup-gcs.png)

### Microsoft Azure

To configure NFS as the backup target, follow these steps:

1. Click **Configs** on the left panel.
2. Click the **Backup** tab.
3. Click **Azure** and enter values for **Container URL** and **SAS Token**.
4. Click **Save**.

![Azure Backup](/images/ee/cloud-provider-configuration-backup-azure.png)