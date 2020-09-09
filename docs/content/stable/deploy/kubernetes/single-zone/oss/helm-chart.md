---
title: Deploy on OSS Kubernetes using Helm Chart 
headerTitle: Open source Kubernetes
linkTitle: Open source Kubernetes
description: Deploy a YugabyteDB cluster on OSS Kubernetes using Helm Chart.
aliases:
  - /stable/deploy/kubernetes/oss/
  - /stable/deploy/kubernetes/oss/helm-chart/
  - /stable/deploy/kubernetes/single-zone/oss/
  - /stable/deploy/kubernetes/helm-chart/
  - /stable/deploy/kubernetes/helm-configuration/
block_indexing: true
menu:
  stable:
    parent: deploy-kubernetes-sz
    name: Open Source
    identifier: k8s-oss-1
    weight: 621
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="{{< relref "./helm-chart.md" >}}" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
  <li >
    <a href="{{< relref "./yugabyte-operator.md" >}}" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      YugabyteDB operator
    </a>
  </li>
  <li >
    <a href="{{< relref "./operator-hub.md" >}}" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Operator Hub
    </a>
  </li>
  <li>
    <a href="{{< relref "./rook-operator.md" >}}" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Rook operator
    </a>
  </li>
</ul>

## Introduction

[Helm](https://helm.sh/) is an open source packaging tool that helps install applications and services on Kubernetes. It uses a packaging format called `charts`. A Helm chart is a package containing all resource definitions necessary to create an instance of a Kubernetes application, tool, or service in a Kubernetes cluster.

## Prerequisites

You must have a Kubernetes cluster that has Helm configured. If you have not installed the Helm client (`helm`), see [Installing Helm](https://helm.sh/docs/intro/install/).

The YugabyteDB Helm chart has been tested with the following software versions:

- Kubernetes 1.14+ with nodes such that a total of 12 CPU cores and 45 GB RAM can be allocated to YugabyteDB. This can be three nodes with 4 CPU core and 15 GB RAM allocated to YugabyteDB.
- Helm 3.0 or later.
- YugabyteDB Docker image (yugabytedb/yugabyte) 2.1.0 or later
- For optimal performance, ensure you've set the appropriate [system limits using `ulimit`](../../../../manual-deployment/system-config/#ulimits) on each node in your Kubernetes cluster.

Confirm that `helm` and `kubectl` are configured correctly.

```sh
$ helm version
```

```
version.BuildInfo{Version:"v3.2.1", GitCommit:"fe51cd1e31e6a202cba7dead9552a6d418ded79a", GitTreeState:"clean", GoVersion:"go1.13.10"}
```

```sh
$ kubectl version
```

## Create cluster

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

### Validate the chart version

```sh
$ helm search repo yugabytedb/yugabyte
```

**Output:**

```sh
NAME               	CHART VERSION	APP VERSION	DESCRIPTION
yugabytedb/yugabyte	2.1.4        	2.1.4.0-b5 	YugabyteDB is the high-performance distributed ...
```

### Install YugabyteDB

Install YugabyteDB in the Kubernetes cluster using the commands below. 

#### On multi-node Kubernetes

Create a namespace and then install YugabyteDB.

```sh
$ kubectl create namespace yb-demo
$ helm install yb-demo yugabytedb/yugabyte --namespace yb-demo --wait
```

#### On Minikube

If you are running in a resource-constrained environment or a local environment, such as Minikube, you will have to change the default resource requirements by using the command below. See next section for a detailed description of these resource requirements.

Create a `yb-demo` namespace.

```sh
$ kubectl create namespace yb-demo
$ helm install yb-demo yugabytedb/yugabyte \
--set resource.master.requests.cpu=0.5,resource.master.requests.memory=0.5Gi,\
resource.tserver.requests.cpu=0.5,resource.tserver.requests.memory=0.5Gi --namespace yb-demo
```

Note that in Minikube, the LoadBalancers for `yb-master-ui` and `yb-tserver-service` will remain in pending state since load balancers are not available in a minikube environment. If you would like to turn off these services simply pass the `enableLoadBalancer=False` flag as shown below.

```sh
$ helm install yb-demo yugabytedb/yugabyte \
--set resource.master.requests.cpu=0.5,resource.master.requests.memory=0.5Gi,\
resource.tserver.requests.cpu=0.5,resource.tserver.requests.memory=0.5Gi,\
enableLoadBalancer=False --namespace yb-demo
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
  kubectl exec --namespace yb-demo -it yb-tserver-0 -- ysqlsh  -h yb-tserver-0.yb-tservers.yb-demo

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
yb-master-0    2/2       Running   0          4m
yb-master-1    2/2       Running   0          4m
yb-master-2    2/2       Running   0          4m
yb-tserver-0   2/2       Running   0          4m
yb-tserver-1   2/2       Running   0          4m
yb-tserver-2   2/2       Running   0          4m
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

You can also check the history of the `yb-demo` deployment.

```sh
$ helm history yb-demo -n yb-demo
```

**Output:**

```sh
REVISION  UPDATED                   STATUS    CHART           APP VERSION   DESCRIPTION
1         Thu Apr 13 13:29:13 2020  deployed  yugabyte-2.1.4 2.1.4.0-b5  Install complete
```

## Connect using YugabyteDB Shells

To connect and use the YSQL Shell (`ysqlsh`), run the following command.

```sh
$ kubectl exec -n yb-demo -it yb-tserver-0 -- ysqlsh -h yb-tserver-0.yb-tservers.yb-demo
```

To connect and use the YCQL Shell (`ycqlsh`), run the following command.

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

Instead of using the default values in the Helm chart, you can also modify the configuration of the YugabyteDB cluster according to your requirements. The following section shows the commands and tags that can be modified to achieve the desired configuration.

### CPU, memory, and replica count

The default values for the Helm chart are in the `helm/yugabyte/values.yaml` file. The most important ones are listed below. As noted in the Prerequisites section above, the defaults are set for a 3-node Kubernetes cluster, each node with 4 CPU cores and 15 GB RAM.

```
persistentVolume:
  count: 2
  storage: 10Gi
  storageClass: standard

resource:
  master:
    requests:
      cpu: 2
      memory: 7.5Gi
  tserver:
    requests:
      cpu: 2
      memory: 7.5Gi

replicas:
  master: 3
  tserver: 3

partition:
  master: 3
  tserver: 3
```

If you want to change the defaults, you can use the command below. You can even do `helm install` instead of `helm upgrade` when you are installing on a Kubernetes cluster with configuration different than the defaults.

```sh
$ helm upgrade --set resource.tserver.requests.cpu=8,resource.tserver.requests.memory=15Gi yb-demo ./yugabyte
```

Replica count can be changed using the command below. Note only the tservers need to be scaled in a Replication Factor 3 cluster which keeps the masters count at `3`.

```sh
$ helm upgrade --set replicas.tserver=5 yb-demo ./yugabyte
```

### Independent LoadBalancers

By default, the YugabyteDB Helm chart will expose the client API endpoints as well as master UI endpoint using 2 LoadBalancers. If you want to expose the client APIs using independent LoadBalancers, you can do the following.

```sh
helm install yb-demo yugabytedb/yugabyte -f https://raw.githubusercontent.com/yugabyte/charts/master/stable/yugabyte/expose-all.yaml --namespace yb-demo --wait
```

You can also bring up an internal LoadBalancer (for either YB-Master or YB-TServer services), if required. Just specify the [annotation](https://kubernetes.io/docs/concepts/services-networking/service/#internal-load-balancer) required for your cloud provider. See [Amazon EKS](../../eks/helm-chart/) and [Google Kubernetes Engine](../../gke/helm-chart/) for examples.

### Storage class

In case you want to use a storage class other than the standard class for your deployment, provision the storage class and then pass in the name of the class while running the helm install command.

```sh
$ helm install yugabyte --namespace yb-demo --name yb-demo --set storage.master.storageClass=<desired storage class>,storage.tserver.storageClass=<desired storage class> --wait
```

### Configure YB-Master and YB-TServer pods

Flags on the yb-master and yb-tserver pods can be done at the command line or by overriding the `values.yaml` file in the charts repository. For example, the following example shows how to set the three geo-distribution related flags of `placement_cloud`,`placement_region` and `placement_zone` on a Minikube cluster.

```sh
helm install yb-demo yugabytedb/yugabyte \
--set resource.master.requests.cpu=0.5,resource.master.requests.memory=0.5Gi,\
resource.tserver.requests.cpu=0.5,resource.tserver.requests.memory=0.5Gi,\
gflags.master.placement_cloud=myk8s-cloud,gflags.master.placement_region=myk8s-region,gflags.master.placement_zone=myk8s-zone,\
gflags.tserver.placement_cloud=myk8s-cloud,gflags.tserver.placement_region=myk8s-region,gflags.tserver.placement_zone=myk8s-zone\
 --namespace yb-demo
```

## Upgrade cluster

You can perform rolling upgrades on the YugabyteDB cluster with the following command. Change the `Image.tag` value to any valid tag from [YugabyteDB's listing on the Docker Hub registry](https://hub.docker.com/r/yugabytedb/yugabyte/tags/). By default, the installation uses the `latest` Docker image. In the examples, the Docker image specified is `2.1.6.0-b17`.

```sh
$ helm upgrade yb-demo yugabytedb/yugabyte --set Image.tag=2.1.6.0-b17 --wait -n yb-demo
```

## Delete cluster

To delete the cluster, you need to purge the Helm chart, and then delete the PVCs.

```sh
$ helm uninstall yb-demo -n yb-demo
```

```sh
$ kubectl delete pvc --namespace yb-demo --all
```
