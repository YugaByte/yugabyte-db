---
title: Configure Kubernetes overrides
headerTitle: Configure Kubernetes overrides
linkTitle: Kubernetes overrides
description: Configure Helm chart overrides for Kubernetes universes in YugabyteDB Anywhere.
aliases:
  - /stable/yugabyte-platform/manage-deployments/edit-helm-overrides/
menu:
  stable_yugabyte-platform:
    identifier: edit-helm-overrides
    parent: scale-deployments
    weight: 30
type: docs
---

If your universe uses Kubernetes, you can set Helm chart overrides when you [create the universe](../../create-deployments/create-universes-wizard/), and modify them later for an existing universe.

To change storage class or volume count on a running universe (v2026.1.0.0 or later), use [Full move for Kubernetes universes](../kubernetes-full-move/) instead.

For provider-level overrides, refer to [Overrides](../../configure-yugabyte-platform/kubernetes/#overrides).

## Configure overrides

### During universe creation

When creating a universe, optionally use the **Helm Overrides** (or **Kubernetes Overrides**) section as follows:

1. Click **Add Kubernetes Overrides** to open the **Kubernetes Overrides** dialog.

1. Using the YAML format (which is sensitive to spacing and indentation), specify the universe-level overrides for YB-Master and YB-TServer, as per the following example:

    ```yaml
    master:
      podLabels:
        service-type: 'database'
    ```

1. Optionally, click **Add Availability Zone** to add availability zone overrides, which only apply to pods that are deployed in that specific availability zone.

    For example, to define overrides for the availability zone us-west-2a, you would click **Add Availability Zone** and use the text area to insert YAML in the following form:

    ```yaml
    us-west-2a:
      master:
        podLabels:
          service-type: 'database'
    ```

    If you specify conflicting overrides, YugabyteDB Anywhere uses the following order of precedence: universe availability zone-level overrides, universe-level overrides, provider overrides.

1. Select **Force Apply** if you want to override any previous overrides.

1. Click **Validate and Save**.

If there are any errors in your overrides definitions, a detailed error message is displayed. You can correct the errors and try to save again. To save your Kubernetes overrides regardless of any validation errors, select **Force Apply**.

### Edit overrides on an existing universe

To edit Kubernetes overrides on an existing universe, navigate to the universe and do the following:

1. {{<tags/ui/new>}} Click **Settings > Advanced**, and under **Helm Overrides** click **Add Helm Overrides** or **Edit**.

    {{<tags/ui/classic>}} Click **Actions > Edit Kubernetes Overrides**.

    This displays the **Kubernetes Overrides** dialog.

1. Complete the dialog using the same YAML format and options described in [During universe creation](#during-universe-creation).

For examples of typical overrides, see [Override examples](#override-examples).

## Override examples

### GKE service account

If you want to enable [GKE service account-based IAM](../../prepare/cloud-permissions/cloud-permissions-nodes-gcp/#gke-service-account-based-iam-gcp-iam) for backup and restore using GCS at the universe level, add the following overrides:

```yaml
tserver:
  serviceAccount: <KSA_NAME>
nodeSelector:
  iam.gke.io/gke-metadata-server-enabled: "true"
```

For end-to-end GCS backup setup on GKE (Workload Identity, storage configuration, and persistence), see [Kubernetes backups (GKE)](../../back-up-restore-universes/configure-backup-storage/#kubernetes-backups-gke) and the GCP **Kubernetes** tab in [Permissions to back up and restore](../../prepare/cloud-permissions/cloud-permissions-storage/).

If you don't provide namespace names for each zone/region during [provider creation](../../configure-yugabyte-platform/kubernetes/), add the names using the following steps:

1. Add the Kubernetes service account to the namespaces where the pods are created.
1. Follow the steps in [Upgrade universes for GKE service account-based IAM](#upgrade-universes-for-gke-service-account-based-iam) to add the annotated Kubernetes service account to pods.

To enable the GKE service account service at the provider level, refer to [Overrides](../../configure-yugabyte-platform/kubernetes/#overrides).

### EKS service account

In AWS, you can attach a service account to database pods; the account can then be used to access storage. The service account used for the database pods should have annotations for the IAM role. The service account to be used can be applied to the DB pods as a Helm override with provider- or universe-level overrides. The IAM role used should be sufficient to access S3 storage.

{{< note title="Node IAM is not pod IAM" >}}
IAM roles on EKS worker nodes do not grant S3 access to pods. Use IRSA on the database pod service account. See [Kubernetes backups (EKS)](../../back-up-restore-universes/configure-backup-storage/#kubernetes-backups-eks) and the AWS **Kubernetes** tab in [Permissions to back up and restore](../../prepare/cloud-permissions/cloud-permissions-storage/).
{{< /note >}}

To enable IAM roles for S3, set the **Use S3 IAM roles attached to DB node for Backup/Restore** Universe Configuration option (config key `yb.backup.s3.use_db_nodes_iam_role_for_backup`) to true. Refer to [Manage runtime configuration settings](../../administer-yugabyte-platform/manage-runtime-config/).

For more information, refer to [Enable IAM roles for service accounts](https://docs.aws.amazon.com/eks/latest/userguide/enable-iam-roles-for-service-accounts.html) in the AWS documentation.

If you want to enable EKS service account-based IAM for backup and restore using S3 at the universe level, add the following overrides:

```yaml
tserver:
  serviceAccount: <KSA_NAME>
```

To enable the EKS service account service at the provider level, refer to [Overrides](../../configure-yugabyte-platform/kubernetes/#overrides).

Save these overrides in YBA (or Operator `kubernetesOverrides`) so they persist across upgrades — see [Make backup settings persistent on Kubernetes](../../back-up-restore-universes/configure-backup-storage/#make-backup-settings-persistent-on-kubernetes).

### Readiness probes

If you want to enable [readiness probes](../../../deploy/kubernetes/single-zone/oss/helm-chart/#readiness-probes), add the following overrides:

```yaml
master:
  readinessProbe:
    enabled: true

tserver:
  readinessProbe:
    enabled: true
```

### Run as root

This override does not apply to OpenShift. OpenShift always runs YugabyteDB pods as non-root and enforces that policy itself, not through `podSecurityContext`.

Starting in YugabyteDB v2026.1.2, YugabyteDB Docker images are STIG-compliant and hardened, and run as a non-root user by default. If you upgrade a non-OpenShift universe that does not pin a user in `podSecurityContext`, the pods migrate to the non-root user from the image.

To keep running as root on a non-OpenShift universe, add the following overrides before you upgrade:

```yaml
podSecurityContext:
  enabled: true
  runAsNonRoot: false
  runAsUser: 0
```

For more information, see [Run as non-root](../../configure-yugabyte-platform/kubernetes/#run-as-non-root).

### ARM VMs

If you want to use ARM VMs, add the following overrides:

```yaml
# Point to the aarch64 image in case multi-arch is not available.
Image:
    tag: {{< yb-version version="stable" format="build">}}-aarch64
# Add a nodeSelector to deploy universe to arm64 nodes in the cluster
nodeSelector:
    kubernetes.io/arch: arm64

# For each master and tserver add tolerations for any taints that might be
# present on the nodes. These taints can be added by default by the
# managed k8s provider or by the cluster administrator
master:
  tolerations:
    - key: kubernetes.io/arch
      operator: Equal
      value: aarch64
      effect: NoSchedule
    - key: kubernetes.io/arch
      operator: Equal
      value: arm64
      effect: NoSchedule
    - key: arch
      operator: Equal
      value: aarch64
      effect: NoSchedule

tserver:
  tolerations:
    - key: kubernetes.io/arch
      operator: Equal
      value: aarch64
      effect: NoSchedule
    - key: kubernetes.io/arch
      operator: Equal
      value: arm64
      effect: NoSchedule
    - key: arch
      operator: Equal
      value: aarch64
      effect: NoSchedule
```

## Create common YB-TServer service for zones

By default, each zone has its own YB-TServer service, and you can use this service to connect to the universe. Optionally, you can create an additional highly available common service across all zones as follows.

Note that this requires all the zone deployments to be in the same namespace.

1. Set the following Kubernetes overrides to add the universe-name label on the YB-TServer pods. You can do this when you [create the universe](#during-universe-creation) or by [editing overrides](#edit-overrides-on-an-existing-universe) on an existing universe.

   ```yaml
   tserver:
     podLabels:
       universe-name: yb-k8s
   ```

1. Save the following to a file named `yb-tserver-common-service.yaml`. You can customize the service type, annotations, and the label selector as required.

   ```yaml
   # yb-tserver-common-service.yaml
   apiVersion: v1
   kind: Service
   metadata:
     name: yb-k8s-common-tserver
     labels:
       app.kubernetes.io/name: yb-tserver
     # annotations:
     #   networking.gke.io/load-balancer-type: "Internal"
   spec:
     type: ClusterIP
     selector:
       app.kubernetes.io/name: yb-tserver
       # This value should match with the value from step 1.
       universe-name: yb-k8s
     ports:
     # Modify the ports if using non-standard ports.
     - name: tcp-yql-port
       port: 9042
     - name: tcp-ysql-port
       port: 5433
   ```

1. Run the following command to create the service in the universe's namespace (`yb-prod-yb-k8s` in this example).

   ```sh
   $ kubectl apply -f yb-tserver-common-service.yaml -n yb-prod-yb-k8s
   ```

After the service YAML is applied, in this example you would access the universe at `yb-k8s-common-tserver.yb-prod-yb-k8s.svc.cluster.local`.

For connecting to Kubernetes universes, refer to [Connect to a universe](../../create-deployments/connect-to-universe/).

## Create a common load balancer service for YB-Masters/YB-TServers

In v2.17 and later, newly created multi-zone universes are deployed in a single namespace by default. This can lead to duplication of load balancer services as a separate load balancer is created for each zone. To prevent creating extra load balancers, you can create a common load balancer service for YB-Masters and YB-TServers that spans all the zones in a namespace.

For scenarios involving multi-namespaces or clusters, a distinct service is created for each namespace, maintaining the flexibility needed for complex deployments while avoiding unnecessary resource allocation.

### Enable the common load balancer

By default, the load balancer service is created per zone. You can change this behavior by [configuring Helm overrides](#configure-overrides) during universe creation, or by enabling a global runtime configuration option.

You can explicitly define the service scope with the values as "AZ" or "Namespaced" when you configure Helm overrides as follows:

```yaml
serviceEndpoints:
  - name: "yb-master-ui"
    type: LoadBalancer
    # Can be AZ/Namespaced
    scope: "AZ"
    annotations: {}
    clusterIP: ""
    ## Sets the Service's externalTrafficPolicy
    externalTrafficPolicy: ""
    app: "yb-master"
    loadBalancerIP: ""
    ports:
      http-ui: "7000"

  - name: "yb-tserver-service"
    type: LoadBalancer
    # Can be AZ/Namespaced
    scope: "AZ"
    annotations: {}
    clusterIP: ""
    ## Sets the Service's externalTrafficPolicy
    externalTrafficPolicy: ""
    app: "yb-tserver"
    loadBalancerIP: ""
    ports:
      tcp-yql-port: "9042"
      tcp-yedis-port: "6379"
      tcp-ysql-port: "5433"
```

For services without an explicitly defined scope in Helm overrides, the default service scope (Namespaced) is used.

Keep in mind the following:

- Scope Utilization: Services with a defined scope will adhere to that scope, as long as it's supported.
- Namespaced scope: For Namespaced-scoped services, a service lifecycle is tied to the lifecycle of the universe.
- Namespace deletion: When a namespace is deleted, all services associated with that namespace that were created by Helm are removed as well.
- Service configuration changes: Existing services can have their serviceType, ports, and annotations updated.

### Migrating service type from AZ to Namespaced scope

After creating a service scope, you can't change it directly. To migrate a service from an AZ scope to a Namespaced scope, do the following:

1. Create a new service: Use Helm overrides to add a new service with the desired Namespaced scope.
1. Migrate clients. Gradually switch clients over to the new Namespaced service to ensure they function correctly without disrupting operations.
1. Remove the old Service. After confirming that all clients are using the new service, update the Helm overrides again to remove the old AZ scoped service.

### Limitations

- Unsupported in YugabyteDB Helm chart versions before v2024.2.
- Unsupported for upgrading universes created prior to v2.18.6.0 and v2.20.2.0.
- Enable exposing service is disabled.

### Examples

To create a universe with Namespaced scope services by default, do the following:

1. When you [configure Helm overrides](#configure-overrides), use serviceEndpoint overrides without explicitly defining scope, or define scope as "Namespaced":

    ```yaml
    serviceEndpoints:
      - name: "yb-master-ui"
        type: LoadBalancer
        annotations: {}
        clusterIP: ""
        ## Sets the Service's externalTrafficPolicy
        externalTrafficPolicy: ""
        app: "yb-master"
        loadBalancerIP: ""
        ports:
          http-ui: "7000"
    ```

Note that irrespective of the default scope, you can add any scope to the services using Helm overrides, provided that the database version supports the scope.

For example, if you create a universe that has "AZ" as the default scope, you can add a "Namespaced" scope as follows:

```yaml
serviceEndpoints:
  - name: "yb-tserver-service"
    type: LoadBalancer
    annotations: {}
    clusterIP: ""
    ## Sets the Service's externalTrafficPolicy
    externalTrafficPolicy: ""
    app: "yb-tserver"
    loadBalancerIP: ""
    ports:
      tcp-yql-port: "9042"
      tcp-yedis-port: "6379"
      tcp-ysql-port: "5433"
  - name: "yb-tserver-service-ns"
    type: LoadBalancer
    # Can be AZ/Namespaced
    scope: "Namespaced"
    annotations: {}
    clusterIP: ""
    ## Sets the Service's externalTrafficPolicy
    externalTrafficPolicy: ""
    app: "yb-tserver"
    loadBalancerIP: ""
    ports:
      tcp-yql-port: "9042"
      tcp-yedis-port: "6379"
      tcp-ysql-port: "5433"
```

## Upgrade universes for GKE service account-based IAM

If you are using Google Cloud Storage (GCS) for backups, you can enable GKE service account-based IAM (GCP IAM) so that Kubernetes universes can access GCS.

Before upgrading a universe for GCP IAM, ensure you have the prerequisites. Refer to [GCP IAM](../../prepare/cloud-permissions/cloud-permissions-nodes-gcp/#gke-service-account-based-iam-gcp-iam).

To upgrade an existing universe to use GCP IAM, do the following:

1. Upgrade YugabyteDB to a version that supports the feature (2.18.4 or later). For more details, refer to [Upgrade the YugabyteDB software](../../manage-deployments/upgrade-software/).

1. Using the steps in [Edit overrides on an existing universe](#edit-overrides-on-an-existing-universe), apply the following overrides.

    - serviceAccount: Provide the name of the Kubernetes service account you created. Note that this service account should be present in the namespace being used for the YugabyteDB pod resources.
    - [nodeSelector](../../install-yugabyte-platform/install-software/kubernetes/#nodeselector): Pass a node selector override to make sure that the YugabyteDB pods are scheduled on the GKE cluster's worker nodes that have a metadata server running.

    ```yaml
    tserver:
      serviceAccount: <KSA_NAME>
    nodeSelector:
      iam.gke.io/gke-metadata-server-enabled: "true"
    ```
