---
title: Cloud setup for backup and restore using YugabyteDB Anywhere
headerTitle: To back up and restore
linkTitle: To back up and restore
description: Prepare your cloud for backup and restore using YugabyteDB Anywhere.
headContent: Prepare your cloud for backup and restore using YugabyteDB Anywhere
menu:
  v2025.1_yugabyte-platform:
    identifier: cloud-permissions-storage
    parent: cloud-permissions
    weight: 30
type: docs
---

When backing up to and/or restoring from external cloud storage, generally speaking, both YugabyteDB Anywhere (YBA) and database nodes require permissions to write to and read from the external storage.

When backing up to an NFS storage target, only database nodes need access to the NFS storage.

<ul class="nav nav-tabs-alt nav-tabs-yb custom-tabs">
  <li>
    <a href="#onprem" class="nav-link active" id="onprem-tab" data-bs-toggle="tab"
      role="tab" aria-controls="onprem" aria-selected="true">
      <i class="fa-solid fa-building"></i>
      On-premises
    </a>
  </li>
  <li>
    <a href="#aws" class="nav-link" id="aws-tab" data-bs-toggle="tab"
      role="tab" aria-controls="aws" aria-selected="false">
      <i class="fa-brands fa-aws"></i>
      AWS
    </a>
  </li>
  <li>
    <a href="#gcp" class="nav-link" id="gcp-tab" data-bs-toggle="tab"
      role="tab" aria-controls="gcp" aria-selected="false">
      <i class="fa-brands fa-google"></i>
      GCP
    </a>
  </li>
  <li>
    <a href="#azure" class="nav-link" id="azure-tab" data-bs-toggle="tab"
      role="tab" aria-controls="azure" aria-selected="false">
      <i class="fa-brands fa-microsoft"></i>
      Azure
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="onprem" class="tab-pane fade show active" role="tabpanel" aria-labelledby="onprem-tab">

When backing up to and/or restoring from NFS storage, the NFS storage system must be configured to allow the following access:

- The `yugabyte` user (and its UID) on the database cluster nodes needs to have read and write permissions for the NFS volume.
- The NFS volume must be mounted on the database cluster nodes.

(This guidance is intentionally repeated in [Prepare Servers for On-Premises provider](../../server-nodes-software/software-on-prem-manual/), where it may be more suitable for some readers.)

  </div>

  <div id="aws" class="tab-pane fade" role="tabpanel" aria-labelledby="aws-tab">

When backing up to and/or restoring from AWS S3 or S3-compatible storage, YBA and database nodes (or pods) must be able to write to and read from the S3 storage bucket.

The following permissions are required:

```properties
"s3:DeleteObject",
"s3:PutObject",
"s3:GetObject",
"s3:ListBucket",
"s3:ListAllMyBuckets",
"s3:GetBucketLocation"
```

<ul class="nav nav-tabs-alt nav-tabs-yb custom-tabs">
  <li>
    <a href="#aws-vm" class="nav-link active" id="aws-vm-tab" data-bs-toggle="tab"
      role="tab" aria-controls="aws-vm" aria-selected="true">
      <i class="fa-solid fa-server"></i>
      VM
    </a>
  </li>
  <li>
    <a href="#aws-k8s" class="nav-link" id="aws-k8s-tab" data-bs-toggle="tab"
      role="tab" aria-controls="aws-k8s" aria-selected="false">
      <i class="fa-regular fa-dharmachakra"></i>
      Kubernetes
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="aws-vm" class="tab-pane fade show active" role="tabpanel" aria-labelledby="aws-vm-tab">

To grant the required access on VMs, do one of the following:

- Provide an IAM user with the permissions listed above, and use its Access key ID and Secret Access Key when creating the backup storage configuration.
- Create the EC2 VM instances (for both the YBA VM and the DB node VMs) with an IAM role that has the required permissions.

| Save for later | To configure |
| :--- | :--- |
| Service account Access key ID and Secret Access Key, or IAM roles on the VMs | [Storage configuration](../../../back-up-restore-universes/configure-backup-storage/#amazon-s3) for S3 |

  </div>

  <div id="aws-k8s" class="tab-pane fade" role="tabpanel" aria-labelledby="aws-k8s-tab">

On Amazon EKS, IAM roles attached to worker **nodes** do not grant S3 access to **pods**. Use [IAM Roles for Service Accounts (IRSA)](https://docs.aws.amazon.com/eks/latest/userguide/iam-roles-for-service-accounts.html) so that a Kubernetes service account (KSA) can assume an IAM role.

**Recommended:** Attach the IAM role to a KSA used by the **database (YBDB) pods** — the universe node IAM path. See [Choose an S3 authentication method](../../../back-up-restore-universes/configure-backup-storage/#choose-an-s3-authentication-method).

| Auth option | Which service account needs the IAM role |
| :--- | :--- |
| Universe node IAM (recommended) | KSA on **YBDB** (tserver) pods. YBA pod IAM is not required for backup I/O. |
| YBA instance IAM (legacy) | KSA on the **YBA** pod (or operator pod). |
| Access Key and Access Secret | Neither. Provide static credentials in the storage configuration. |

##### Create an IRSA-enabled service account

1. Create an IAM role with the [S3 permissions](#aws) listed above, and a trust policy that allows your EKS OIDC provider to assume the role for the KSA. For example:

    ```json
    {
      "Version": "2012-10-17",
      "Statement": [
        {
          "Effect": "Allow",
          "Principal": {
            "Federated": "arn:aws:iam::<ACCOUNT_ID>:oidc-provider/<OIDC_PROVIDER>"
          },
          "Action": "sts:AssumeRoleWithWebIdentity",
          "Condition": {
            "StringEquals": {
              "<OIDC_PROVIDER>:sub": "system:serviceaccount:<NAMESPACE>:<KSA_NAME>",
              "<OIDC_PROVIDER>:aud": "sts.amazonaws.com"
            }
          }
        }
      ]
    }
    ```

1. Create the KSA in each namespace where database pods run, and annotate it with the role ARN:

    ```yaml
    apiVersion: v1
    kind: ServiceAccount
    metadata:
      name: <KSA_NAME>
      namespace: <NAMESPACE>
      annotations:
        eks.amazonaws.com/role-arn: arn:aws:iam::<ACCOUNT_ID>:role/<IAM_ROLE_NAME>
    ```

1. Apply the KSA to database pods using provider or universe [Helm overrides](../../../create-deployments/create-universe-multi-zone-kubernetes/#eks-service-account) (or Operator `kubernetesOverrides`). Do not rely on one-off `kubectl edit` changes — those are lost on upgrade. See [Make backup settings persistent on Kubernetes](../../../back-up-restore-universes/configure-backup-storage/#make-backup-settings-persistent-on-kubernetes).

##### Verify from a database pod

After the universe is running with the annotated KSA:

```sh
kubectl exec -n <NAMESPACE> -it <TSERVER_POD> -- \
  aws sts get-caller-identity
```

Confirm that the returned ARN matches the IRSA role you created.

| Save for later | To configure |
| :--- | :--- |
| Annotated KSA name and IAM role | [Kubernetes backups (EKS)](../../../back-up-restore-universes/configure-backup-storage/#kubernetes-backups-eks) and [EKS service account](../../../create-deployments/create-universe-multi-zone-kubernetes/#eks-service-account) overrides |

  </div>
</div>

  </div>

  <div id="gcp" class="tab-pane fade" role="tabpanel" aria-labelledby="gcp-tab">

When backing up to and/or restoring from GCP GCS, YBA and database nodes (or pods) must be able to write to and read from the GCS storage bucket.

The following permissions are required:

```sh
roles/storage.admin
```

<ul class="nav nav-tabs-alt nav-tabs-yb custom-tabs">
  <li>
    <a href="#gcp-vm" class="nav-link active" id="gcp-vm-tab" data-bs-toggle="tab"
      role="tab" aria-controls="gcp-vm" aria-selected="true">
      <i class="fa-solid fa-server"></i>
      VM
    </a>
  </li>
  <li>
    <a href="#gcp-k8s" class="nav-link" id="gcp-k8s-tab" data-bs-toggle="tab"
      role="tab" aria-controls="gcp-k8s" aria-selected="false">
      <i class="fa-regular fa-dharmachakra"></i>
      Kubernetes
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="gcp-vm" class="tab-pane fade show active" role="tabpanel" aria-labelledby="gcp-vm-tab">

To grant the required access on VMs, do one of the following:

- Provide a GCP service account with [IAM roles for cloud storage](https://cloud.google.com/storage/docs/access-control/iam-roles) with the permissions listed above, and use its JSON credentials when creating the backup storage configuration.
- Create the VM instances (for both the YBA VM and the DB node VMs) with an IAM role that has the required permissions.

| Save for later | To configure |
| :--- | :--- |
| Service account JSON credentials, or IAM roles on the VMs | [Storage configuration](../../../back-up-restore-universes/configure-backup-storage/#google-cloud-storage) for GCS |

  </div>

  <div id="gcp-k8s" class="tab-pane fade" role="tabpanel" aria-labelledby="gcp-k8s-tab">

On Google Kubernetes Engine (GKE), use [Workload Identity](https://cloud.google.com/kubernetes-engine/docs/how-to/workload-identity) so that a Kubernetes service account (KSA) can act as a Google IAM service account. Node-level service accounts do not replace Workload Identity for pods.

**Recommended:** Annotate a KSA used by the **database (YBDB) pods**, and enable **Use GCP IAM** on the storage configuration. For background, see [GKE service account-based IAM](../cloud-permissions-nodes-gcp/#gke-service-account-based-iam-gcp-iam).

| Auth option | Which service account needs the IAM binding |
| :--- | :--- |
| Use GCP IAM with YBDB pods (recommended) | KSA on **YBDB** (tserver) pods, bound to a Google IAM service account with `roles/storage.admin` (or equivalent). |
| Use GCP IAM on the YBA pod | KSA on the **YBA** pod (set via Helm at install/upgrade). Often used together with YBDB pod IAM so YBA can validate the bucket. |
| GCS JSON credentials | Neither. Provide JSON credentials in the storage configuration. |

##### Prerequisites

- Workload Identity enabled on the GKE cluster; worker nodes have the GKE metadata server enabled.
- A Google IAM service account with permission to read, write, list, and delete objects in GCS (`roles/storage.admin` or equivalent).
- The annotated KSA present in every namespace where YBA and/or YBDB pods run.

##### Annotate the Kubernetes service account

Bind the KSA to the Google IAM service account:

```yaml
apiVersion: v1
kind: ServiceAccount
metadata:
  name: <KSA_NAME>
  namespace: <NAMESPACE>
  annotations:
    iam.gke.io/gcp-service-account: <GSA_NAME>@<PROJECT_ID>.iam.gserviceaccount.com
```

Also grant the Google IAM service account permission for the KSA to impersonate it (see [Use Workload Identity](https://cloud.google.com/kubernetes-engine/docs/how-to/workload-identity) in the GKE documentation).

Apply the KSA to database pods using provider or universe [Helm overrides](../../../create-deployments/create-universe-multi-zone-kubernetes/#gke-service-account), including the metadata-server nodeSelector. Persist YBA-pod IAM via Helm `values.yaml` — see [Enable GKE service account-based IAM](../../../install-yugabyte-platform/install-software/kubernetes/#enable-gke-service-account-based-iam) and [Make backup settings persistent on Kubernetes](../../../back-up-restore-universes/configure-backup-storage/#make-backup-settings-persistent-on-kubernetes).

##### Verify from a database pod

```sh
kubectl exec -n <NAMESPACE> -it <TSERVER_POD> -- \
  curl -s -H "Metadata-Flavor: Google" \
  http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/email
```

Confirm that the email matches the Google IAM service account bound to the KSA.

| Save for later | To configure |
| :--- | :--- |
| Annotated KSA name and Google IAM service account | [Kubernetes backups (GKE)](../../../back-up-restore-universes/configure-backup-storage/#kubernetes-backups-gke) and [GKE service account](../../../create-deployments/create-universe-multi-zone-kubernetes/#gke-service-account) overrides |

  </div>
</div>

  </div>

  <div id="azure" class="tab-pane fade" role="tabpanel" aria-labelledby="azure-tab">

When backing up to and/or restoring from Azure Storage, YBA and DB nodes must be able to write to and read from the storage blob.

To grant the required access, create a [Shared Access Signature (SAS)](https://learn.microsoft.com/en-us/azure/storage/common/storage-sas-overview) token with the permissions as shown in the following illustration.

![Azure Shared Access Signature page](/images/yp/cloud-provider-configuration-backup-azure-generate-token.png)

The Connection string and SAS token are used when creating a backup [storage configuration](../../../back-up-restore-universes/configure-backup-storage/#azure-storage) for Azure.

| Save for later | To configure |
| :--- | :--- |
| Azure storage Connection string and SAS token | [Storage configuration](../../../back-up-restore-universes/configure-backup-storage/#azure-storage) for Azure |

  </div>

</div>
