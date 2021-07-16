---
title: Install Yugabyte Platform Software - OpenShift
headerTitle: Install Yugabyte Platform Software - OpenShift
linkTitle: Install software 
description: Install Yugabyte Platform software in your OpenShift environment
menu:
  latest:
    parent: install-yugabyte-platform
    identifier: install-software-2-openshift
    weight: 77
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/install-software/default" class="nav-link">
      <i class="fas fa-cloud"></i>
      Default
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/install-software/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li >
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/install-software/airgapped" class="nav-link">
      <i class="fas fa-unlink"></i>
      Airgapped
    </a>
  </li>

<li >
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/install-software/openshift" class="nav-link active">
      <i class="fas fa-cubes"></i> OpenShift </a>
  </li>

</ul>

To install Yugabyte Platform on an OpenShift cluster, you can use Yugabyte Platform Operator or the Helm tool.

## Prerequisites

Before you install Yugabyte Platform on an OpenShift cluster, you need to prepare the environment, as described in [Prepare the OpenShift Environment](../../../install-yugabyte-platform/prepare-environment/openshift/).

Unless otherwise specified, you can use a user account for executing the steps described in this document. Using admin account for all the steps should work as well.

## Operator-Based Installation

Installing Yugabyte Platform on an OpenShift cluster using the Yugabyte Platform Operator involves the following:

- [Installing the Operator itself](#installing-the-operator)
- [Creating an instance of Yugabyte Platform](#creating-an-instance-of-yugabyte-platform-via-operator)
- [Finding the availability zone labels](#finding-the-availability-zone-labels)
- [Configuring the CLI with the OCP cluster](#configuring-the-cli-with-the-ocp-cluster)
- [Accessing and configuring Yugabyte Platform](#accessing-and-configuring-yugabyte-platform)
- Optionally, [upgrading the Yugabyte Platform instance](#upgrading-the-yugabyte-platform-instance)


### Installing the Operator

You can install the Yugabyte Platform Operator via the OpenShift web console or command line.

#### How to Use the OpenShift Web Console

You can install the Yugabyte Platform Operator as follows: 

- Login to the OpenShift Container Platform (OCP) cluster’s web console using admin credentials (for example, kube:admin).
- Navigate to the **Operators > OperatorHub**, search for Yugabyte Platform Operator, and then open it to display details about the operator, as shown in the following illustration:

![Operator](/images/ee/openshift-operator.png)

- Click **Install**.
- Accept default settings on the **Install Operator** page, as shown in the following illustration, and then click **Install**.

![Install Operator](/images/ee/openshift-install-operator.png)

Once the installation is complete, a message shown in the following illustration is displayed:

![Operator Installed](/images/ee/openshift-operator-installed.png)

#### How to Use the Command Line

Alternatively, you can install the operator via the command line. You start by configuring oc with an admin account (kube:admin) and following the procedure described in [Configuring oc with the OCP Cluster](#configuring-the-cli-with-the-ocp-cluster).

To install the Yugabyte Platform Operator, execute the following command:

```shell
oc apply -f - <<EOF
apiVersion: operators.coreos.com/v1alpha1
kind: Subscription
metadata:
  name: yugabyte-platform-operator-bundle
  namespace: openshift-operators
spec:
  channel: alpha
  name: yugabyte-platform-operator-bundle
  source: certified-operators
  sourceNamespace: openshift-marketplace
EOF
```

This creates a Subscription object and installs the operator in the cluster, as demonstrated by the following output:

```
# output
subscription.operators.coreos.com/yugabyte-platform-operator-bundle created
```

To verify that the operator pods are in Running state, execute the following command:

```shell
oc get pods -n openshift-operators | grep -E '^NAME|yugabyte-platform'
```

```
# output
NAME                                                         READY  STATUS  RESTARTS  AGE
yugabyte-platform-operator-controller-manager-7485db7486-6nzxr 2/2  Running  0      5m38s
```

For additional information, see [Adding Operators to a Cluster](https://docs.openshift.com/container-platform/4.6/operators/admin/olm-adding-operators-to-cluster.html).

### Creating an Instance of Yugabyte Platform via Operator

You start by creating an instance of Yugabyte Platform in a new project (namespace) called yb-platform. To do this, you can use the OpenShift web console or command line.

#### How to Use the OpenShift Web Console

You can create an instance of Yugabyte Platform via the OpenShift web console as follows:

- Open the OCP web console and navigate to **Home > Projects > Create Project**.
- Enter the name yb-platform and click **Create**.
- Navigate to **Operators > Installed Operators** and select **Yugabyte Platform Operator**, as shown in the following illustration:

![Yugabyte Platform Install Operator](/images/ee/openshift-install-yp-operator.png)

- Click **Create Instance** to open the **Create YBPlatform** page. 

- Ensure that the **yb-platform** project is selected and review the default settings.

- Accept the default settings without modifying them, unless your cluster has a StorageClass other than standard, in which case open **YAML View** and change the value of `spec.yugaware.storageClass` to the correct StorageClass name.

  You can find the StorageClass by navigating to **Storage > Storage Classes** on the OpenShift Web Console as admin user.

- Click **Create**. 

Shortly, you should expect the **Status** column in the **Yugabyte Platform** tab to display **Deployed**, as shown in the following illustration:

![Yugabyte Platform Installed Operator](/images/ee/openshift-yp-operator-platforms.png)

#### How to Use the Command Line

Alternatively, you can create an instance of Yugabyte Platform via the command line, as follows:

- To create a new project, execute the following command:

  ```shell
  oc new-project yb-platform
  ```
  ```
  # output
  Now using project "yb-platform" on server "web-console-address"
  ```

- Verify the StorageClass setting for your cluster by executing the following command as admin user:
  ```shell
  oc get storageClass
  ```

  If your cluster's StorageClass is not `standard`, change the value of   `spec.yugaware.storageClass` to the correct StorageClass name when you create an instance of Yugabyte Platform.

- To create an instance of Yugabyte Platform in the yb-platform project, execute the following command:

  ```shell
  oc apply \
    -n yb-platform \
    -f - <<EOF
  apiVersion: yugabyte.com/v1alpha1
  kind: YBPlatform
  metadata:
    name: ybplatform-sample
  spec:
    image:
      repository: registry.connect.redhat.com/yugabytedb/yugabyte-platform
      tag: latest
    ocpCompatibility:
      enabled: true
    rbac:
      create: false
  EOF
  ```
  ```
  # output
  ybplatform.yugabyte.com/ybplatform-sample created
  ```

- To verify that the pods of the Yugabyte Platform instance are in Running state, execute the following:

  ```shell
  oc get pods -n yb-platform -l app=ybplatform-sample-yugaware
  ```
  ```
  # output
  NAME                         READY  STATUS  RESTARTS  AGE
  Ybplatform-sample-yugaware-0  5/5   Running  0        22s
  ```


### Upgrading the Yugabyte Platform Instance

You may choose to upgrade the Yugabyte Platform instance installed using the Operator to a new tag that you receive from Yugabyte. In the current release, you can do this by using the command line. 

The following example shows the command you would execute to update the container image tag to 2.5.2.0-b89:

```shell
oc patch \
 ybplatform ybplatform-sample \
 -p '{"spec":{"image":{"tag":"2.5.2.0-b89"}}}' --type merge \
 -n yb-platform
```

```
# output
ybplatform.yugabyte.com/ybplatform-sample patched
```

To verify that the pods are being updated, execute the following command:

```shell
oc get pods -n yb-platform -l app=ybplatform-sample-yugaware -w
```

```
# output
NAME                         READY  STATUS          RESTARTS  AGE
ybplatform-sample-yugaware-0  5/5   Running            0     18m
ybplatform-sample-yugaware-0  0/5   Terminating        0     19m
ybplatform-sample-yugaware-0  0/5   Pending            0     0s
ybplatform-sample-yugaware-0  0/5   ContainerCreating  0     35s
ybplatform-sample-yugaware-0  5/5   Running            0     93s
```

## Helm-Based Installation

In addition to meeting the requirements described in  [Prepare the OpenShift Environment](../../../install-yugabyte-platform/prepare-environment/openshift/), you need to perform the following steps before attempting to install Yugbyte Platform using Helm:

- Verify that the OpenShift cluster is configured with Helm 3 or later by executing the following command:

  ```shell
  $ helm version
  ```

  The output should be similar to the following:

  ```
  version.BuildInfo{Version:"v3.2.1", GitCommit:"fe51cd1e31e6a202cba7dead9552a6d418ded79a", GitTreeState:"clean", GoVersion:"go1.13.10"
  ```

- Ensure that an OpenShift secret license file has been obtained from Yugabyte Support.

Installing Yugabyte Platform on an OpenShift cluster using Helm involves the following:

- [Creating an instance of Yugabyte Platform](#creating-an-instance-of-yugabyte-platform-via-helm)
- [Finding the availability zone labels](#finding-the-availability-zone-labels)
- [Configuring the CLI with the OCP cluster](#configuring-the-cli-with-the-ocp-cluster)
- [Accessing and configuring Yugabyte Platform](#accessing-and-configuring-yugabyte-platform)

### Creating an Instance of Yugabyte Platform via Helm

To create a Yugabyte Platform instance, perform the following:

- Create a new project (namespace) called yb-platform by executing the following command:

  ```shell
  oc new-project yb-platform
  ```

  ```
  # output
  Now using project "yb-platform" on server "web-console-address"
  ```

- Apply the Yugabyte Platform secret that you obtained from Yugabyte Support by executing the following command:

  ```shell
  oc create -f yugabyte-k8s-secret.yml -n yb-platform 
  ```

  ```
  # output
  secret/yugabyte-k8s-pull-secret created 
  ```

- Execute the following command to add the [YugabyteDB charts](https://charts.yugabyte.com/) repository:

  ```shell
  helm repo add yugabytedb https://charts.yugabyte.com
  ```

  ```
  # output
  "yugabytedb" has been added to your repositories
  ```

  To search for the available chart version, execute the following command:

  ```shell
  helm search repo yugabytedb/yugaware -l 
  ```

  ```
  # output
  NAME              CHART VERSION  APP VERSION   DESCRIPTION                    
  yugabytedb/yugaware   2.5.3      2.5.3.1-b10   YugaWare is YugaByte Database's...  
  ```

- Verify the StorageClass setting for your cluster by executing the following command as admin user:

  ```shell
  oc get storageClass
  ```
  
  If your cluster's StorageClass is not `standard`, add `--set yugaware.storageClass=<storage-class-name>` when installing the Yugabyte Platform Helm chart in the next step.

- Execute the following command to install the Yugabyte Platform Helm chart:

  ```shell
  helm install yw-test yugabytedb/yugaware -n yb-platform \
     --set image.tag=latest-ubi \
     --set ocpCompatibility.enabled=true --set rbac.create=false --wait
  ```

  Expect to see a message notifying you whether or not the deployment is successful.

  Note that if you are executing the preceding command as an admin user, then you can set `rbac.create=true`. Alternatively, you can ask the cluster administrator to perform the next step.
  
- Optionally, execute the following command as an admin user to create ClusterRoleBinding:

  ```shell
  oc apply -f - <<EOF
  kind: ClusterRoleBinding
  apiVersion: rbac.authorization.k8s.io/v1
  metadata:
    name: yw-test-cluster-monitoring-view
    labels:
      app: yugaware
  subjects:
  - kind: ServiceAccount
    name: yw-test
    namespace: yb-platform
  roleRef:
    kind: ClusterRole
    name: cluster-monitoring-view
    apiGroup: rbac.authorization.k8s.io
  EOF
  ```

### Deleting the Helm Installation of Yugabyte Platform

You can delete the Helm installation by executing the following command:

```shell
helm delete yw-test -n yb-platform
```

## Finding the Availability Zone Labels

You need to find the region name and availability zone codes where the cluster is running. This information is required by Yugabyte Platform (see [Creating a Provider in Yugabyte Platform](../../../configure-yugabyte-platform/set-up-cloud-provider/openshift#creating-a-provider-in-yugabyte-platform)). For example, if your OCP cluster is in the US East, then the cloud provider's zone labels can be us-east4-a, us-east4-b, and so on.

You can use the OpenShift web console or the command line to search for the availability zone codes.

### How to Use the OpenShift Web Console

You start by logging in the OCP's web console as admin user, and then performing the following:

- Navigate to **Compute > Machine Sets** and open each Machine Set.

- On the **Machine Set Details** page, open the **Machines** tab to access the region and availability zone label, as shown in the following illustration where the region is US East and the availability zone label is us-east4-a.

  ![Create Machines](/images/ee/openshift-yp-create-machine.png)

### How to Use the Command Line

Alternatively, you can find the availability zone codes via the command line.

You start by configuring oc with an admin account (kube:admin) and following the procedure described in [Configuring oc with the OCP Cluster](#configuring-oc-with-the-ocp-cluster).

To find the region and zone labels, execute the following command:

```shell
oc get machinesets \
  -n openshift-machine-api \
  -ojsonpath='{range .items[*]}{.metadata.name}{", region: "}{.spec.template.spec.providerSpec.value.region}{", zone: "}{.spec.template.spec.providerSpec.value.zone}{"\n"}{end}'
```

```
# output
  ocp-dev4-l5ffp-worker-a, region: us-east4, zone: us-east4-a
  ocp-dev4-l5ffp-worker-b, region: us-east4, zone: us-east4-b
  ocp-dev4-l5ffp-worker-c, region: us-east4, zone: us-east4-c
```

After the execution, the region is displayed as US East and the zones as us-east4-a, us-east4-b, and so on.

## Configuring the CLI with the OCP Cluster

To configure the OpenShift command-line interface (CLI) tool oc, you start by logging in to the OCP web console with your user account. For more information and specific instructions, see [Getting Started with the CLI](https://docs.openshift.com/container-platform/4.2/cli_reference/openshift_cli/getting-started-cli.html).

## Accessing and Configuring Yugabyte Platform

Once you have created and deployed Yugabyte Platform, you can access its web UI and create an account.

### How to Find the Location to Access the Web UI

To find the location (IP address or hostname), you can use the OpenShift web console or the command line.

#### Using the OpenShift Web Console

You can obtain the location using the OpenShift web console as follows:

- Use the OCP web console to navigate to **Networking > Services** and select **ybplatform-sample-yugaware-ui** from the list. Ensure that the **yb-platform** project is selected.
- In the **Service Routing** section of the **Details** tab, locate **External Load Balancer** and copy the value, as shown in the following illustration:

![Service Details](/images/ee/openshift-service-details.png)

- Open the copied location in a new instance of your web browser.

#### Using the Command Line

Alternatively, you can obtain the information about the location via the command line.

In case of the Operator-based installation of Yugabyte Platform, execute the following command: 

```shell
oc get services \
  ybplatform-sample-yugaware-ui \
  -ojsonpath='{.status.loadBalancer.ingress[0].ip}{"\n"}
              {.status.loadBalancer.ingress[0].hostname}{"\n"}'
```

```
# output
12.34.56.78
```

In case of the Helm-based installation, execute the following command: 

```shell
oc get services \
   yw-test-yugaware-ui \
  -ojsonpath='{.status.loadBalancer.ingress[0].ip}{"\n"}
              {.status.loadBalancer.ingress[0].hostname}{"\n"}'
```

```
# output
12.34.56.78
```

