---
title: Configure backup storage
headerTitle: Configure backup storage
linkTitle: Configure backup storage
description: Configure backup storage
headContent: Store your backups in the cloud or on NFS
menu:
  v2025.1_yugabyte-platform:
    parent: back-up-restore-universes
    identifier: configure-backup-storage
    weight: 10
rightNav:
  hideH4: true
type: docs
---

Before you can back up universes, you need to configure a storage location for your backups.

Depending on your environment, you can save your YugabyteDB universe data to a variety of storage solutions.

## Amazon S3

You can configure AWS S3 and S3-compatible storage as your backup target.

### Prerequisites

- S3-compatible storage requires S3 path style access.

    By default, the option to use S3 path style access is not available.

    To enable the feature in YugabyteDB Anywhere, set the **Enable Path Access Style for Amazon S3** Global Runtime Configuration option (config key `yb.ui.feature_flags.enable_path_style_access`) to true. Refer to [Manage runtime configuration settings](../../administer-yugabyte-platform/manage-runtime-config/). Note that only a Super Admin user can modify Global configuration settings.

- To validate connections to your S3 storage using custom self-signed or CA certificates, add the certificates to the YugabyteDB Anywhere Trust Store. Refer to [Add certificates to your trust store](../../security/enable-encryption-in-transit/trust-store/).

### Choose an S3 authentication method

When you create an S3 backup configuration, choose how YugabyteDB Anywhere and the universe authenticate to the bucket. Enable **IAM Role** for either IAM path, or leave it disabled and provide static credentials.

**Recommended default:** For production universes on AWS (including EKS with IRSA), use universe node IAM roles. Use static **Access Key** and **Access Secret** credentials for evaluation setups, or when neither the YBA host nor the universe nodes can assume IAM roles. Treat YBA instance IAM as legacy, and keep it only for existing configurations that already depend on it.

| Option | When to use | Prerequisites | Trade-offs |
| :--- | :--- | :--- | :--- |
| Universe node IAM role {{<tags/feature/ea>}} | Production backups on AWS VMs or Kubernetes (EKS IRSA). Recommended for new deployments. | Attach IAM roles (or annotated Kubernetes service accounts) with the [required S3 IAM permissions](#required-s3-iam-permissions) to each universe node or database pod.<br>Set the **Use S3 IAM roles attached to DB node for Backup/Restore** [Universe Configuration option](../../administer-yugabyte-platform/manage-runtime-config/) to true. Enable **IAM Role** on the storage configuration. For EKS, see [Kubernetes backups (EKS)](#kubernetes-backups-eks). | YB Controller on each node authenticates to S3 directly, so backups scale with cluster size. Requires a universe-level runtime configuration change. Preferred path going forward. |
| YBA instance IAM role (Legacy) {{<tags/feature/ga>}} | Existing setups that already use the IAM role attached to the YugabyteDB Anywhere VM or pod. | Attach an IAM role with the [required S3 IAM permissions](#required-s3-iam-permissions) to the YugabyteDB Anywhere host. Enable **IAM Role**. <br>Leave **Use S3 IAM roles attached to DB node for Backup/Restore** at the default (`false`). | YBA fetches temporary credentials from its own instance role and passes them to database nodes. This path does not scale well for large backups and is not recommended for new deployments. |
| Access Key and Access Secret {{<tags/feature/ga>}} | Evaluation and proof-of-concept setups; environments where neither YBA nor universe nodes have IAM roles (including many S3-compatible targets). | An IAM user (or equivalent) with the [required S3 IAM permissions](#required-s3-iam-permissions). Leave **IAM Role** disabled and enter **Access Key** and **Access Secret**. | Simplest path to a first successful backup. Credentials are stored in YBA and must be rotated manually. Prefer IAM for production when available. |

#### Examples

- **First backup / evaluation.** Leave **IAM Role** disabled. Enter an AWS access key and secret for a user that can read and write the backup bucket.
- **New production AWS or EKS universe.** Attach instance profiles (or IRSA service accounts) to the database nodes, set **Use S3 IAM roles attached to DB node for Backup/Restore** (`yb.backup.s3.use_db_nodes_iam_role_for_backup`) to true for the universe, then enable **IAM Role** on the storage configuration.
- **Existing YBA-role configuration.** Keep **IAM Role** enabled and leave `yb.backup.s3.use_db_nodes_iam_role_for_backup` at `false` until you can migrate nodes to their own IAM roles.

For cloud permission setup details, refer to [Permissions to back up and restore](../../prepare/cloud-permissions/cloud-permissions-storage/). On EKS, open the AWS **Kubernetes** tab on that page for IRSA setup.

### Create an AWS backup configuration

To configure S3 storage, do the following:

1. Navigate to **Integrations** > **Backup** > **Amazon S3**.

1. Click **Create S3 Backup**.

    ![S3 Backup](/images/yp/cloud-provider-configuration-backup-aws.png)

1. Use the **Configuration Name** field to provide a meaningful name for your storage configuration.

1. Choose authentication using the guidance in [Choose an S3 authentication method](#choose-an-s3-authentication-method):

    - To use IAM (universe node IAM role or legacy YBA instance IAM role), enable **IAM Role**.

        For universe node IAM roles, also set the **Use S3 IAM roles attached to DB node for Backup/Restore** Universe Configuration option (config key `yb.backup.s3.use_db_nodes_iam_role_for_backup`) to true. Refer to [Manage runtime configuration settings](../../administer-yugabyte-platform/manage-runtime-config/).

    - To use static credentials, leave **IAM Role** disabled and enter values for the **Access Key** and **Access Secret** fields.

        For information on AWS access keys, see [Manage access keys for IAM users](https://docs.aws.amazon.com/IAM/latest/UserGuide/id_credentials_access-keys.html).

1. In the **S3 Bucket** field, enter the bucket name in the format `s3://bucket_name`, or `https://storage_vendor/s3-bucket-name` for S3-compatible storage.

1. In the **S3 Bucket Host Base** field, enter the HTTP host header (endpoint URL) of the AWS S3 or S3-compatible storage, in the form `s3.amazonaws.com` or `my.storage.com`.

1. If you are using S3-compatible storage, set the **S3 Path Style Access** option to true. (The option is only available after you enable the **Enable Path Access Style for Amazon S3** Global Runtime Configuration option, config key `yb.ui.feature_flags.enable_path_style_access`.)

1. Click **Save**.

You can configure access control for the S3 bucket as follows:

- Provide the required access control list (ACL), and then define **List, Write** permissions to access **Objects**, as well as **Read, Write** permissions for the bucket, as shown in the following illustration:

    ![S3](/images/yp/backup-aws-access-control.png)

- Create Bucket policy to enable access to the objects stored in the bucket.

### Required S3 IAM permissions

The following S3 IAM permissions are required:

```properties
"s3:DeleteObject",
"s3:PutObject",
"s3:GetObject",
"s3:ListBucket",
"s3:ListAllMyBuckets",
"s3:GetBucketLocation"
```

### Kubernetes backups (EKS)

{{<tags/feature/ea>}}On Amazon EKS, use IRSA so database pods can assume an IAM role for S3. Node instance profiles alone do **not** grant S3 access to pods.

For IAM role trust policy, KSA annotations, and verification steps, refer to the AWS **Kubernetes** tab in [Permissions to back up and restore](../../prepare/cloud-permissions/cloud-permissions-storage/).

To configure S3 backups with universe node IAM on EKS:

1. Create an IRSA-enabled Kubernetes service account (KSA) with the [required S3 IAM permissions](#required-s3-iam-permissions), in each namespace where database pods run.

1. Attach the KSA to database pods using provider or universe Helm overrides. For example:

    ```yaml
    tserver:
      serviceAccount: <KSA_NAME>
    ```

    For details, see [EKS service account](../../create-deployments/create-universe-multi-zone-kubernetes/#eks-service-account). If you use the [YugabyteDB Kubernetes Operator](../../anywhere-automation/yb-kubernetes-operator/), set the same override under `spec.kubernetesOverrides`.

1. Set the **Use S3 IAM roles attached to DB node for Backup/Restore** Universe Configuration option (config key `yb.backup.s3.use_db_nodes_iam_role_for_backup`) to true. Refer to [Manage runtime configuration settings](../../administer-yugabyte-platform/manage-runtime-config/).

1. Create an S3 storage configuration with **IAM Role** enabled, as described in [Create an AWS backup configuration](#create-an-aws-backup-configuration).

1. Verify credentials from a tserver pod (see the prepare page), then run a test backup.

For which settings survive pod restarts and upgrades, see [Make backup settings persistent on Kubernetes](#make-backup-settings-persistent-on-kubernetes).

### Make backup settings persistent on Kubernetes

Settings you apply with `kubectl edit`, one-off interactive install prompts, or temporary pod changes are **not** durable. Use the following so backup IAM configuration survives restarts and upgrades:

| Setting | Persistent if you… | Not persistent if you… |
| :--- | :--- | :--- |
| Storage configuration (**IAM Role**, Access Key / Secret, GCS credentials) | Create or update it in the YBA UI or API (stored in the YBA database). | — |
| **Use S3 IAM roles attached to DB node for Backup/Restore** (`yb.backup.s3.use_db_nodes_iam_role_for_backup`) | Set it via YBA [runtime configuration](../../administer-yugabyte-platform/manage-runtime-config/) (UI or API). | — |
| Database pod `serviceAccount` (and GKE `nodeSelector`) | Set provider or universe **Helm overrides** in YBA, or Operator `kubernetesOverrides`, then apply/upgrade the universe. | Edit the live Deployment or Pod with `kubectl`, or change the SA only in the cluster without updating overrides. |
| YBA pod service account (legacy YBA IAM / GKE YBA Workload Identity) | Put `yugaware.serviceAccount` (and any required `nodeSelector`) in your Helm `values.yaml` and run `helm upgrade`. | Rely only on interactive install answers that were never written to `values.yaml`. |
| IRSA / Workload Identity annotations on the KSA | Keep annotations in the KSA manifest you manage (GitOps, Helm chart for the SA, or `kubectl apply` of that manifest). | Recreate the KSA without annotations, or expect YBA to recreate IRSA bindings for you. |

#### Universe pod overrides (recommended path)

Set the database pod service account in YBA (provider- or universe-level overrides), for example:

```yaml
tserver:
  serviceAccount: <KSA_NAME>
```

For GKE, also include:

```yaml
nodeSelector:
  iam.gke.io/gke-metadata-server-enabled: "true"
```

Saving these overrides in YBA and applying them to the universe is what makes the pod identity persistent. See [Helm overrides](../../create-deployments/create-universe-multi-zone-kubernetes/#helm-overrides).

#### YBA Helm values (YBA pod IAM)

If the YBA pod itself must use a cloud IAM role (legacy S3 path, or GKE validation), set the service account in values and upgrade:

```yaml
yugaware:
  serviceAccount: <KSA_NAME>
nodeSelector:
  iam.gke.io/gke-metadata-server-enabled: "true"   # GKE only
```

```sh
helm upgrade <RELEASE_NAME> yugabytedb/yugaware -n <YBA_NAMESPACE> -f values.yaml
```

For GKE install-time guidance, see [Enable GKE service account-based IAM](../../install-yugabyte-platform/install-software/kubernetes/#enable-gke-service-account-based-iam).

### Using a proxy

By default, **Proxy Configuration** for S3 storage is not available in the UI. To make it available, navigate to `https://<my-yugabytedb-anywhere-ip>/features` and enable the **enableS3BackupProxy** option.

Configure a proxy for your S3 backup configuration by setting the following options under **Proxy Configuration**:

- **Host**: The full URL or IP address of the HTTP/HTTPS proxy server.
- **Port**: The port used by the HTTP/HTTPS proxy server.
- **Username** and **Password**: If your proxy requires authentication, enter the Username and Password.

## Google Cloud Storage

You can configure Google Cloud Storage (GCS) as your backup target.

### Required GCP service account permissions

To grant access to your bucket, create a GCP service account with [IAM roles for cloud storage](https://cloud.google.com/storage/docs/access-control/iam-roles) with the following permissions:

```sh
roles/storage.admin
```

The credentials for this account (in JSON format) are used when creating the backup storage configuration with static credentials. For information on how to obtain GCS credentials, see [Cloud Storage authentication](https://cloud.google.com/storage/docs/authentication).

You can configure access control for the GCS bucket as follows:

- Provide the required access control list (ACL) and set it as either uniform or fine-grained (for object-level access).
- Add permissions, such as roles and members.

### Choose a GCS authentication method

When you create a GCS backup configuration, choose how YugabyteDB Anywhere and the universe authenticate to the bucket:

- **Use GCP IAM** — Use Workload Identity (GKE) or the IAM identity on the YBA / database host. On GKE, attach a Google IAM service account to the Kubernetes service account used by database pods (and typically the YBA pod). See [Kubernetes backups (GKE)](#kubernetes-backups-gke).
- **GCS Credentials (JSON)** — Simplest path for evaluation setups, or when Workload Identity / host IAM is unavailable. Leave **Use GCP IAM** disabled and paste the service account JSON.

For cloud permission setup, refer to the GCP tab in [Permissions to back up and restore](../../prepare/cloud-permissions/cloud-permissions-storage/) (use the **Kubernetes** sub-tab for GKE).

### Create a GCS backup configuration

To create a GCP backup configuration, do the following:

1. Navigate to **Integrations > Backup > Google Cloud Storage**.

1. Click **Create GCS Backup**.

    ![GCS Configuration](/images/yp/cloud-provider-configuration-backup-gcs-stable.png)

1. Use the **Configuration Name** field to provide a meaningful name for your storage configuration.

1. Enter the URI of your GCS bucket in the **GCS Bucket** field. For example, `gs://gcp-bucket/test_backups`.

1. Choose authentication:

    - To use IAM (including GKE Workload Identity), select **Use GCP IAM**.
    - Otherwise, leave **Use GCP IAM** disabled and enter the credentials for your account in JSON format in the **GCS Credentials** field.

1. Click **Save**.

### Kubernetes backups (GKE)

On GKE, use [Workload Identity](https://cloud.google.com/kubernetes-engine/docs/how-to/workload-identity) so database pods can access GCS. For KSA annotations, IAM bindings, and verification, refer to the GCP **Kubernetes** tab in [Permissions to back up and restore](../../prepare/cloud-permissions/cloud-permissions-storage/) and [GKE service account-based IAM](../../prepare/cloud-permissions/cloud-permissions-nodes-gcp/#gke-service-account-based-iam-gcp-iam).

To configure GCS backups with Workload Identity:

1. Create a Google IAM service account with the [required permissions](#required-gcp-service-account-permissions), and a Kubernetes service account (KSA) annotated for Workload Identity in each namespace where pods run.

1. Attach the KSA to database pods using provider or universe Helm overrides:

    ```yaml
    tserver:
      serviceAccount: <KSA_NAME>
    nodeSelector:
      iam.gke.io/gke-metadata-server-enabled: "true"
    ```

    For details, see [GKE service account](../../create-deployments/create-universe-multi-zone-kubernetes/#gke-service-account). To upgrade an existing universe, see [Upgrade universes for GKE service account-based IAM](../../manage-deployments/edit-helm-overrides/#upgrade-universes-for-gke-service-account-based-iam).

1. If the YBA pod must also use Workload Identity (recommended so YBA can validate the bucket), set the YBA Helm service account as described in [Enable GKE service account-based IAM](../../install-yugabyte-platform/install-software/kubernetes/#enable-gke-service-account-based-iam), and persist it with `helm upgrade` — see [Make backup settings persistent on Kubernetes](#make-backup-settings-persistent-on-kubernetes).

1. Create a GCS storage configuration with **Use GCP IAM** enabled, as described in [Create a GCS backup configuration](#create-a-gcs-backup-configuration).

1. Verify the workload identity from a tserver pod, then run a test backup.

## Azure Storage

You can configure Azure as your backup target.

### Prerequisites

- Azure storage account.
- [Blob container](https://learn.microsoft.com/en-us/azure/storage/blobs/storage-quickstart-blobs-portal#create-a-container).
- [SAS Token](https://learn.microsoft.com/en-us/azure/storage/common/storage-sas-overview?toc=/azure/storage/blobs/toc.json&bc=/azure/storage/blobs/breadcrumb/toc.json).

### Create an Azure storage configuration

In YugabyteDB Anywhere:

1. Navigate to **Integrations > Backup > Azure Storage**.

1. Click **Create AZ Backup**.

    ![Azure Configuration](/images/yp/cloud-provider-configuration-backup-azure.png)

1. Use the **Configuration Name** field to provide a meaningful name for your storage configuration.

1. Enter the **Container URL** of the container you created. You can obtain the container URL in Azure by navigating to **Container > Properties**.

1. Provide the **SAS Token** you generated. You can copy the SAS Token directly from **Shared access signature** page in Azure.

1. Click **Save**.

## Network File System

You can configure Network File System (NFS) as your backup target, as follows:

1. Navigate to **Integrations > Backup > Network File System**.

1. Click **Create NFS Backup** to access the configuration form shown in the following illustration:

    ![NFS Configuration](/images/yp/cloud-provider-configuration-backup-nfs.png)

1. Use the **Configuration Name** field to provide a meaningful name for your storage configuration.

1. Complete the **NFS Storage Path** field by entering `/backup` or another directory that provides read, write, and access permissions to the SSH user of the YugabyteDB Anywhere instance.

1. Click **Save**.

{{< warning title="Prevent back up failure due to NFS unmount on cloud VM restart" >}}
To avoid potential backup and restore errors, add the NFS mount to `/etc/fstab` on the nodes of universes using the backup configuration. When a cloud VM is restarted, the NFS mount may get unmounted if its entry is not in `/etc/fstab`. This can lead to backup failures, and errors during [backup](../back-up-universe-data/) or [restore](../restore-universe-data/).
{{< /warning >}}

## Local storage

If your YugabyteDB universe has one node, you can create a local directory on a YB-TServer to which to back up, as follows:

1. Navigate to **Universes**, select your universe, and then select **Nodes**.

1. Click **Connect**.

1. Take note of the services and endpoints information displayed in the **Connect** dialog, as shown in the following illustration:

    ![Connect dialog](/images/yp/cloud-provider-local-backup1.png)

1. While connected using `ssh`, create a directory `/backup` and then change the owner to `yugabyte`, as follows:

    ```sh
    sudo mkdir /backup; sudo chown yugabyte /backup
    ```

If there is more than one node, you should consider using a [network file system](#network-file-system) mounted on each server.
