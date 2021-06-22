---
title: Configure backup storage
headerTitle: Configure backup storage
linkTitle: Configure backup storage
description: Configure backup storage
menu:
  latest:
    parent: back-up-restore-universes
    identifier: configure-backup-storage
    weight: 10
isTocNested: true
showAsideToc: true
---

Depending on your environment, you can save your YugabyteDB universe data to a variety of storage solutions.

## Amazon S3

You can configure Amazon S3 as your backup target as follows:

1. Navigate to **Configs** > **Backup** > **amazon S3**.

2. Enter values for **Access Key** and **Access Secret** fields. 

   You may use Identity Access Management (IAM).

3. Enter values for the **S3 Bucket** and **S3 Bucket Host Base** fields.

   For information on how to obtain AWS credentials, see [Understanding and getting your AWS credentials](https://docs.aws.amazon.com/general/latest/gr/aws-sec-cred-types.html).

4. Click **Save**.

![AWS Backup](/images/yp/cloud-provider-configuration-backup-aws.png)

You can configure access control for the S3 bucket as follows: 

- Provide the required access control list (ACL), and then define **List, Write** permissions to access **Objects**, as well as **Read, Write** permissions for the bucket, as shown in the following illustration: <br><br>
  ![img](/images/yp/backup-aws-access-control.png)
- Create Bucket policy to enable access to the objects stored in the bucket.

## Network File Share

You can configure Network File Share (NFS) as your backup target as follows:

1. Navigate to **Configs** > **Backup** > **NFS**.
3. Complete the **NFS Storage Path** field by entering `/backup` or another directory that provides read, write, and access permissions to the SSH user of the Yugabyte Platform instance.
3. Click **Save**.

![NFS Cloud Provider Configuration](/images/yp/cloud-provider-configuration-backup-nfs.png)

## Google Cloud Storage

You can configure Google Cloud Storage (GCS) as your backup target as follows:

1. Navigate to **Configs** > **Backup** > **GCS**.

3. Complete  **GCS Bucket** and **GCS Credentials** fields.

   For information on how to obtain GCS credentials, see [Cloud Storage authentication](https://cloud.google.com/storage/docs/authentication).

4. Click **Save**.

![GCS Backup](/images/yp/cloud-provider-configuration-backup-gcs.png)

You can configure access control for the S3 bucket as follows: 

- Provide the required access control list (ACL) and set it as either uniform or fine-grained (for object-level access).
- Add permissions, such as roles and members.

## Microsoft Azure

You can configure Azure as your backup target as follows:

1. Create a storage account in Azure as follows:

    <br/>

    * Navigate to **Portal > Storage Account** and click **Add** (+).
    * Complete the mandatory fields, such as **Resource group**, **Storage account name**, and **Location**, as per the following illustration:

    <br/>

    ![Azure storage account creation](/images/yp/cloud-provider-configuration-backup-azure-account.png)

1. Create a blob container as follows:

    <br/>

    * Open the storage account (for example, **storagetestazure**, as shown in the following illustration).
    * Navigate to **Blob service > Containers > + Container** and then click **Create**.

    <br/>

    ![Azure blob container creation](/images/yp/cloud-provider-configuration-backup-azure-blob-container.png)

1. Obtain the container URL by navigating to **Container > Properties**, as shown in the following illustration:<br>

    <br/>

    ![Azure container properties](/images/yp/cloud-provider-configuration-backup-azure-container-properties.png)

1. Generate an SAS Token as follows:

    <br/>

    * Navigate to **Storage account > Shared access signature**, as shown in the following illustration.
    * Under **Allowed resource types**, select **Container** and **Object**.
    * Click **Generate SAS and connection string** and copy the SAS token. Note that the token should start with `?sv=`.

    <br/>

    ![Azure Shared Access Signature page](/images/yp/cloud-provider-configuration-backup-azure-generate-token.png)

1. On your Yugabyte Platform instance, provide the container URL and SAS token for creating a backup, as follows:

    <br/>

    * Navigate to **Configs** > **Backup** > **Azure**.
    * Enter values for the **Container URL** and **SAS Token** fields, as shown in the following illustration, and then click **Save**.<br><br>
    
    ![Azure Backup](/images/yp/cloud-provider-configuration-backup-azure.png)
