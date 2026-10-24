---
title: Cloud setup for deploying universe nodes on Kubernetes
headerTitle: To deploy nodes
linkTitle: To deploy nodes
description: Prepare your cloud for deploying universe nodes using a Kubernetes provider configuration.
headContent: Prepare your cloud for deploying YugabyteDB universe nodes
menu:
  stable_yugabyte-platform:
    identifier: cloud-permissions-nodes-6-k8s
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
    <a href="../cloud-permissions-nodes-gcp/" class="nav-link">
      <i class="fa-brands fa-google"></i>
      GCP
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-azure/" class="nav-link">
      <i class="fa-brands fa-microsoft"></i>
      Azure
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-oci/" class="nav-link">
      <i class="icon-oracle" aria-hidden="true"></i>
      OCI
    </a>
  </li>
  <li>
    <a href="../cloud-permissions-nodes-k8s/" class="nav-link active">
      <i class="fa-regular fa-dharmachakra"></i>
      Kubernetes
    </a>
  </li>
</ul>

For YugabyteDB Anywhere (YBA) to deploy and manage YugabyteDB universes on Kubernetes, you need to provide YBA with a service account that can create and manage pods, services, StatefulSets, secrets, and related resources in the target namespaces. The more permissions that you can provide, the more YBA can automate.

## Kubernetes

As a prerequisite for creating pods and deploying database clusters, YBA requires a service account in the Kubernetes cluster and a corresponding kubeconfig file.

For S3 or GCS backup IAM on Kubernetes (which pods need roles, IRSA / Workload Identity, and how to make settings persistent), see [Permissions to back up and restore](../cloud-permissions-storage/) (AWS or GCP **Kubernetes** tab) and [Configure backup storage](../../../back-up-restore-universes/configure-backup-storage/#make-backup-settings-persistent-on-kubernetes).

Do one of the following:

- Create a [`yugabyte-platform-universe-management` service account](#create-a-service-account) and a [`kubeconfig` file](#create-a-kubeconfig-file) directly.

  Do this for each Kubernetes cluster if you are doing a multi-cluster setup. Prefer this approach when you need least-privilege access (for example, Namespace Restricted), because the YBA installation service account may not have the permissions required to manage universes.

- If deploying into a single Kubernetes cluster **and** the service account used for your YBA installation already has sufficient permissions to manage universes, have YBA auto-fill these values using that service account and kubeconfig.

  See [Create a provider](../../../configure-yugabyte-platform/kubernetes/) for details.

### Create a service account

YBA uses a kubeconfig generated for this service account to authenticate to the Kubernetes cluster. Do not delete the account after YBA starts using it.

Set the `YBA_NAMESPACE` environment variable to the namespace where you will create the service account. This is commonly the namespace where YBA is installed, but it can be any namespace.

```sh
export YBA_NAMESPACE="yb-platform"
```

Note that the `YBA_NAMESPACE` variable is used in the commands throughout this document.

Run the following `kubectl` command to apply the YAML file:

```sh
export YBA_NAMESPACE="yb-platform"

kubectl apply -f https://raw.githubusercontent.com/yugabyte/charts/master/rbac/yugabyte-platform-universe-management-sa.yaml -n ${YBA_NAMESPACE}
```

Expect the following output:

```output
serviceaccount/yugabyte-platform-universe-management created
```

The next step is to grant access to this service account using ClusterRoles and Roles, as well as ClusterRoleBindings and RoleBindings, so it can manage YugabyteDB universe resources.

In the RBAC manifests, `<SA_NAMESPACE>` is the namespace where the service account lives (the value of `YBA_NAMESPACE`). For Namespace Admin and Namespace Restricted, that is separate from the *target* namespaces where you deploy universes; those are set with `kubectl apply -n`.

The tasks you can perform depend on your access level.

**Global Admin** can grant broad cluster-level admin access by executing the following command:

```sh
export YBA_NAMESPACE="yb-platform"

curl -s https://raw.githubusercontent.com/yugabyte/charts/master/rbac/platform-global-admin.yaml \
  | sed "s/namespace: <SA_NAMESPACE>/namespace: ${YBA_NAMESPACE}/g" \
  | kubectl apply -f -
```

**Global Restricted** can grant access to only the specific cluster roles required to create and manage YugabyteDB universes across all namespaces in a cluster. The `platform-global.yaml` manifest contains ClusterRoles and ClusterRoleBindings for the required set of permissions.

```sh
export YBA_NAMESPACE="yb-platform"

curl -s https://raw.githubusercontent.com/yugabyte/charts/master/rbac/platform-global.yaml \
  | sed "s/namespace: <SA_NAMESPACE>/namespace: ${YBA_NAMESPACE}/g" \
  | kubectl apply -f -
```

**Namespace Admin** can grant namespace-level admin access. Contains a RoleBinding that grants the `cluster-admin` ClusterRole to the service account in each target namespace.

Apply the YAML in each target namespace where you want to deploy universes. For example, to grant namespace-level admin access in `yb-db-demo` and `yb-db-us-east4-a`:

```sh
export YBA_NAMESPACE="yb-platform"

for ns in yb-db-demo yb-db-us-east4-a; do
  curl -s https://raw.githubusercontent.com/yugabyte/charts/master/rbac/platform-namespaced-admin.yaml \
    | sed "s/namespace: <SA_NAMESPACE>/namespace: ${YBA_NAMESPACE}/g" \
    | kubectl apply -n "${ns}" -f -
done
```

**Namespace Restricted** can grant access to only the specific roles required to create and manage YugabyteDB universes in a particular namespace. Contains Roles and RoleBindings for the required set of permissions.

As with Namespace Admin, apply the YAML in each target namespace. For example, if your goal is to allow YBA to manage YugabyteDB universes in the namespaces `yb-db-demo` and `yb-db-us-east4-a`, apply in both target namespaces as follows:

```sh
export YBA_NAMESPACE="yb-platform"

for ns in yb-db-demo yb-db-us-east4-a; do
  curl -s https://raw.githubusercontent.com/yugabyte/charts/master/rbac/platform-namespaced.yaml \
    | sed "s/namespace: <SA_NAMESPACE>/namespace: ${YBA_NAMESPACE}/g" \
    | kubectl apply -n "${ns}" -f -
done
```

### Create a kubeconfig file

You can create a `kubeconfig` file for the previously created `yugabyte-platform-universe-management` service account as follows.

The `-n` argument must be the namespace where the service account was created (`YBA_NAMESPACE`), not a target universe namespace.

1. Run the following `wget` command to get the Python script for generating the `kubeconfig` file:

    ```sh
    wget https://raw.githubusercontent.com/yugabyte/charts/master/stable/yugabyte/generate_kubeconfig.py
    ```

1. Run the following command to generate the `kubeconfig` file. The script creates a service account token secret (required on Kubernetes 1.24 and later) and writes a kubeconfig that uses that token:

    ```sh
    export YBA_NAMESPACE="yb-platform"

    python3 generate_kubeconfig.py -s yugabyte-platform-universe-management -n ${YBA_NAMESPACE}
    ```

    Expect the following output:

    ```output
    Generated the kubeconfig file: /tmp/yugabyte-platform-universe-management.conf
    ```

1. Use this generated `kubeconfig` file for your Kubernetes provider configuration.

| Save for later | To configure |
| :--- | :--- |
| kubeconfig file | [Kubernetes provider](../../../configure-yugabyte-platform/kubernetes/) |
