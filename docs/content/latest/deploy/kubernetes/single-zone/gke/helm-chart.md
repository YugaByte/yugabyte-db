---
title: Use Helm Chart to deploy on Google Kubernetes Engine (GKE)
headerTitle: Google Kubernetes Engine (GKE)
linkTitle: Google Kubernetes Engine (GKE)
description: Use Helm Chart to deploy a single-zone YugabyteDB cluster on Google Kubernetes Engine (GKE).
menu:
  latest:
    parent: deploy-kubernetes-sz
    name: Google Kubernetes Engine
    identifier: k8s-gke-1
    weight: 623
aliases:
  - /latest/deploy/kubernetes/gke/
  - /latest/deploy/kubernetes/gke/helm-chart/
  - /latest/deploy/kubernetes/single-zone/gke/
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/gke/helm-chart" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/gke/statefulset-yaml" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      YAML (remote disk)
    </a>
  </li>
   <li >
    <a href="/latest/deploy/kubernetes/single-zone/gke/statefulset-yaml-local-ssd" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      YAML (local disk)
    </a>
  </li>
</ul>

## Prerequisites

You must have a GKE cluster that has Helm configured. If you have not installed the Helm client (`helm`), see [Installing Helm](https://helm.sh/docs/intro/install/).

The YugabyteDB Helm Chart has been tested with the following software versions:

- GKE running Kubernetes 1.14+ with nodes such that a total of 12 CPU cores and 45 GB RAM can be allocated to YugabyteDB. This can be three nodes with 4 CPU core and 15 GB RAM allocated to YugabyteDB. `n1-standard-8` is the minimum instance type that meets these criteria.
- Helm 3.0 or later
- Docker image for YugabyteDB (`yugabytedb/yugabyte`) 2.1.0 or later
- For optimal performance, ensure you set the appropriate [system limits using `ulimit`](../../../../manual-deployment/system-config/#ulimits) on each node in your Kubernetes cluster.

The following steps show how to meet these prerequisites.

- Download and install the [Google Cloud SDK](https://cloud.google.com/sdk/downloads/).

- Configure defaults for gcloud

Set the project ID as `yugabyte`. You can change this as needed.

```sh
$ gcloud config set project yugabyte
```

Set the default compute zone as `us-west1-b`. You can change this as needed.

```sh
$ gcloud config set compute/zone us-west1-b
```

- Install `kubectl`

After installing Cloud SDK, install the `kubectl` command line tool by running the following command.

```sh
$ gcloud components install kubectl
```

Note that GKE is usually 2 or 3 major releases behind the upstream/OSS Kubernetes release. This means you have to make sure that you have the latest kubectl version that is compatible across different Kubernetes distributions if that's what you intend to.

- Ensure `helm` is installed

First, check to see if Helm is installed by using the Helm version command.

```sh
$ helm version
```

You should see something similar to the following output. Note that the `tiller` server side component has been removed in Helm 3.

```
version.BuildInfo{Version:"v3.0.3", GitCommit:"ac925eb7279f4a6955df663a0128044a8a6b7593", GitTreeState:"clean", GoVersion:"go1.13.6"}
```

## 1. Create a GKE cluster

Create a Kubernetes cluster, if you have not already done so, by running the following command.

```sh
$ gcloud container clusters create yugabyte --machine-type=n1-standard-8
```

As stated in [Prerequisites](#prerequisites) above, the default configuration in the YugabyteDB Helm Chart requires Kubernetes nodes to have a total of 12 CPU cores and 45 GB RAM allocated to YugabyteDB. This can be three nodes with 4 CPU cores and 15 GB RAM allocated to YugabyteDB. The smallest Google Cloud machine type that meets this requirement is `n1-standard-8` which has 8 CPU cores and 30GB RAM.

## 2. Create a YugabyteDB cluster

### Add charts repository

To add the YugabyteDB charts repository, run the following command.

```sh
$ helm repo add yugabytedb https://charts.yugabyte.com
```

### Fetch updates from the repository

Make sure that you have the latest updates to the repository by running the following command.

```sh
$ helm repo update
```

### Validate the Chart version

```sh
$ helm search repo yugabytedb/yugabyte
```

**Output:**

```sh
NAME                CHART VERSION APP VERSION DESCRIPTION                                       
yugabytedb/yugabyte 2.1.4         2.1.4.0-b5  YugabyteDB is the high-performance distributed ...
```

### Install YugabyteDB

Run the following commands to create a namespace and then install Yugabyte.

```sh
$ kubectl create namespace yb-demo
$ helm install yb-demo yugabytedb/yugabyte --namespace yb-demo --wait
```

## Check the cluster status

You can check the status of the cluster using various commands noted below.

```sh
$ helm status yb-demo -n yb-demo
```

**Output**:

```sh
NAME: yb-demo
LAST DEPLOYED: Thu Feb 13 13:29:13 2020
NAMESPACE: yb-demo
STATUS: deployed
REVISION: 1
TEST SUITE: None
NOTES:
1. Get YugabyteDB Pods by running this command:
  kubectl --namespace yb-demo get pods

2. Get list of YugabyteDB services that are running:
  kubectl --namespace yb-demo get services

3. Get information about the load balancer services:
  kubectl get svc --namespace yb-demo

4. Connect to one of the tablet server:
  kubectl exec --namespace yb-demo -it yb-tserver-0 -- bash

5. Run YSQL shell from inside of a tablet server:
  kubectl exec --namespace yb-demo -it yb-tserver-0 -- ysqlsh -h yb-tserver-0.yb-tservers.yb-demo

6. Cleanup YugabyteDB Pods
  helm delete yb-demo --purge
  NOTE: You need to manually delete the persistent volume
  kubectl delete pvc --namespace yb-demo -l app=yb-master
  kubectl delete pvc --namespace yb-demo -l app=yb-tserver
```

Check the pods.

```sh
$ kubectl get pods --namespace yb-demo
```

```
NAME           READY     STATUS    RESTARTS   AGE
yb-master-0    1/1       Running   0          4m
yb-master-1    1/1       Running   0          4m
yb-master-2    1/1       Running   0          4m
yb-tserver-0   1/1       Running   0          4m
yb-tserver-1   1/1       Running   0          4m
yb-tserver-2   1/1       Running   0          4m
```

Check the services.

```sh
$ kubectl get services --namespace yb-demo
```

```
NAME                 TYPE           CLUSTER-IP      EXTERNAL-IP    PORT(S)                                        AGE
yb-master-ui         LoadBalancer   10.109.39.242   35.225.153.213 7000:31920/TCP                                 10s
yb-masters           ClusterIP      None            <none>         7100/TCP,7000/TCP                              10s
yb-tserver-service   LoadBalancer   10.98.36.163    35.225.153.214 6379:30929/TCP,9042:30975/TCP,5433:30048/TCP   10s
yb-tservers          ClusterIP      None            <none>         7100/TCP,9000/TCP,6379/TCP,9042/TCP,5433/TCP   10s
```

You can even check the history of the `yb-demo` deployment.

```sh
$ helm history yb-demo -n yb-demo
```

**Output:**

```sh
REVISION  UPDATED                   STATUS    CHART           APP VERSION DESCRIPTION     
1         Tue Apr 21 17:29:01 2020  deployed  yugabyte-2.1.4  2.1.4.0-b5  Install complete
```

## Connect using YugabyteDB shells

To connect and use the YSQL Shell `ysqlsh`, run the following command.

```sh
$ kubectl exec -n yb-demo -it yb-tserver-0 -- ysqlsh -h yb-tserver-0.yb-tservers.yb-demo
```

To connect and use the YCQL Shell `ycqlsh`, run the following command.

```sh
$ kubectl exec -n yb-demo -it yb-tserver-0 -- ycqlsh yb-tserver-0.yb-tservers.yb-demo
```

## Connect using external clients

To connect an external program, get the load balancer `EXTERNAL-IP` IP address of the `yb-tserver-service` service and connect to the 5433 / 9042 ports for YSQL / YCQL services respectively.

```sh
$ kubectl get services --namespace yb-demo
```

```
NAME                 TYPE           CLUSTER-IP      EXTERNAL-IP   PORT(S)                                        AGE
...
yb-tserver-service   LoadBalancer   10.98.36.163    35.225.153.214     6379:30929/TCP,9042:30975/TCP,5433:30048/TCP   10s
...
```

## Configure cluster

You can configure the cluster using the same commands and options as [Open Source Kubernetes](../../oss/helm-chart/#configure-cluster).

### Independent LoadBalancers

By default, the YugabyteDB Helm Chart will expose the client API endpoints as well as master UI endpoint using two LoadBalancers. If you want to expose the client APIs using independent LoadBalancers, you can do the following.

```sh
helm install yb-demo yugabytedb/yugabyte -f https://raw.githubusercontent.com/yugabyte/charts/master/stable/yugabyte/expose-all.yaml --namespace yb-demo --wait
```

You can also bring up an internal LoadBalancer (for either YB-Master or YB-TServer services), if required. Just specify the [annotation](https://kubernetes.io/docs/concepts/services-networking/service/#internal-load-balancer) required for your cloud provider. The following command brings up an internal LoadBalancer for the YB-TServer service in Google Cloud Platform.

```sh
$ helm install yugabyte -f https://raw.githubusercontent.com/yugabyte/charts/master/stable/yugabyte/expose-all.yaml --namespace yb-demo --name yb-demo \
  --set annotations.tserver.loadbalancer."cloud\.google\.com/load-balancer-type"=Internal --wait
```
