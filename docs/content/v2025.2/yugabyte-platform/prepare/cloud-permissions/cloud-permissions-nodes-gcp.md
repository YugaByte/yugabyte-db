---
title: Cloud setup for deploying universe nodes on GCP
headerTitle: To deploy nodes
linkTitle: To deploy nodes
description: Prepare your cloud for deploying universe nodes using an GCP provider configuration.
headContent: Prepare your cloud for deploying YugabyteDB universe nodes
menu:
  v2025.2_yugabyte-platform:
    identifier: cloud-permissions-nodes-3-gcp
    parent: cloud-permissions
    weight: 20
type: docs
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="../cloud-permissions-nodes/" class="nav-link">
      <i class="fa-solid fa-building"></i>
      On-premises
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-aws/" class="nav-link">
      <i class="fa-brands fa-aws"></i>
      AWS
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-gcp" class="nav-link active">
      <i class="fa-brands fa-google"></i>
      GCP
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-azure" class="nav-link">
      <i class="fa-brands fa-microsoft"></i>
      Azure
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-k8s" class="nav-link">
      <i class="fa-regular fa-dharmachakra"></i>
      Kubernetes
    </a>
  </li>
</ul>

For YugabyteDB Anywhere (YBA) to be able to deploy and manage YugabyteDB universes using a GCP [cloud provider configuration](../../../yba-overview/#public-cloud), you need to provide YBA with privileges on your cloud infrastructure to create, delete, and modify VMs, mount and unmount disk volumes, and so on. The more permissions that you can provide, the more YBA can automate.

{{<tip>}}
If you can't provide YBA with the necessary permissions, you can still deploy to GCP using an [on-premises provider](../cloud-permissions-nodes/).
{{</tip>}}

## GCP

The [Compute Admin role](https://cloud.google.com/compute/docs/access/iam#compute.admin) permission is required on the GCP service account where you will deploy:

```sh
roles/compute.admin
```

To grant the required access, you must do the following:

- Create a service account in GCP.
- [Grant the required roles](https://cloud.google.com/iam/docs/grant-role-console) to the account.

Then use one of the following methods:

- Obtain a file containing a JSON that describes the service account credentials. You will need to provide this file later when creating the GCP provider configuration.
- [Attach the service account](https://cloud.google.com/compute/docs/access/create-enable-service-accounts-for-instances#using) to the GCP VM that will run YBA.

| Save for later | To configure |
| :--- | :--- |
| Service account JSON | [GCP provider configuration](../../../configure-yugabyte-platform/gcp/) |

## Managing SSH keys for VMs

When creating VMs on the public cloud using a [cloud provider configuration](../../../yba-overview/#public-cloud), YugabyteDB requires SSH keys to access the VM. You can manage the SSH keys for VMs in two ways:

- YBA managed keys. When YBA creates VMs, it will generate and manage the SSH key pair.
- Provide a custom key pair. Create your own custom SSH keys and upload the SSH keys when you create the provider.

The key pair authenticates as the image's default login user. For the user requirements, see [Software requirements for cloud provider nodes](../../server-nodes-software/software-cloud-provider/).

If you will be using your own custom SSH keys, ensure they are authorized for that user and that you have them when installing YBA and creating your GCP cloud provider configuration.

| Save for later | To configure |
| :--- | :--- |
| Custom SSH keys | [GCP provider configuration](../../../configure-yugabyte-platform/gcp/) |

## GKE service account-based IAM (GCP IAM)

Google Kubernetes Engine (GKE) uses a concept known as "Workload Identity" to provide a secure way to allow a Kubernetes service account ([KSA](https://kubernetes.io/docs/concepts/security/service-accounts/)) in your GKE cluster to act as an IAM service account so that your Kubernetes universes can access GCS for backups.

In GKE, each pod can be associated with a KSA. The KSA is used to authenticate and authorize the pod to interact with other Google Cloud services. An IAM service account is a Google Cloud resource that allows applications to make authorized calls to Google Cloud APIs.

Workload Identity links a KSA to an IAM account using annotations in the KSA. Pods that use the configured KSA automatically authenticate as the IAM service account when accessing Google Cloud APIs.

By using Workload Identity, you avoid the need for manually managing service account keys or tokens in your applications running on GKE. This approach enhances security and simplifies the management of credentials.

- To enable GCP IAM when installing YugabyteDB Anywhere on Kubernetes, refer to [Enable GKE service account-based IAM](../../../install-yugabyte-platform/install-software/kubernetes/#enable-gke-service-account-based-iam).

- To prepare GCS permissions and Workload Identity for backups, refer to the GCP **Kubernetes** tab in [Permissions to back up and restore](../cloud-permissions-storage/).

- To enable GCP IAM during universe creation on Kubernetes, refer to [Configure Helm overrides](../../../create-deployments/create-universe-multi-zone-kubernetes/#helm-overrides).

- To enable GCP IAM for Google Cloud Storage backup configuration with Kubernetes, refer to [Kubernetes backups (GKE)](../../../back-up-restore-universes/configure-backup-storage/#kubernetes-backups-gke).

- To upgrade an existing universe with GCP IAM, refer to [Upgrade universes for GKE service account-based IAM support](../../../manage-deployments/edit-helm-overrides/#upgrade-universes-for-gke-service-account-based-iam).

**Prerequisites**

- The GKE cluster hosting the pods should have Workload Identity enabled. The worker nodes of this GKE cluster should have the GKE metadata server enabled.

- The IAM service account, which is used to annotate the KSA, should have sufficient permissions to read, write, list, and delete objects in GCS.

- The KSA, which is annotated with the IAM service account, should be present in the same namespace where the pod resources for YugabyteDB Anywhere and YugabyteDB universes are expected. If you have multiple namespaces, each namespace should include the annotated KSA.

For instructions on setting up Workload Identity, see [Use Workload Identity](https://cloud.google.com/kubernetes-engine/docs/how-to/workload-identity) in the GKE documentation.
