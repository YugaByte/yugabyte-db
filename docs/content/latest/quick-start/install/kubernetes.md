---
title: Install YugabyteDB on Kubernetes
headerTitle: 1. Install YugabyteDB
linkTitle: 1. Install YugabyteDB
description: Download and install YugabyteDB on Kubernetes in less than five minutes.
aliases:
  - /quick-start/install/
menu:
  latest:
    parent: quick-start
    name: 1. Install YugabyteDB
    identifier: install-4-kubernetes
    weight: 110
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/quick-start/install/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/install/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/install/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/latest/quick-start/install/kubernetes" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

## Prerequisites

- [Minikube](https://github.com/kubernetes/minikube) is installed on your localhost machine.

The Kubernetes version used by Minikube should be v1.13.0 or later. The default Kubernetes version being used by Minikube displays when you run the `minikube start` command. To install Minikube, see [Install Minikube](https://kubernetes.io/docs/tasks/tools/install-minikube/) in the Kubernetes documentation.

- [kubectl](https://kubernetes.io/docs/reference/kubectl/overview/) is installed.

To install `kubectl`, see [Install kubectl](https://kubernetes.io/docs/tasks/tools/install-kubectl/) in the Kubernetes documentation.

- [Helm 3+](https://helm.sh/) is installed. If you have Helm 2 then make sure you have Tiller installed on the Kubernetes cluster and thereafter change the helm commands accordingly.

To install `helm`, see [Install helm](https://helm.sh/docs/intro/install/) in the Helm documentation.

## Start Kubernetes

- Start Kubernetes using Minikube by running the following command. Note that minikube by default brings up a single-node Kubernetes environment with 2GB RAM, 2 CPUS, and a disk of 20GB. We recommend starting minkube with at least 8GB RAM, 4 CPUs and 40GB disk as shown below.

```sh
$ minikube start --memory=8192 --cpus=4 --disk-size=40g --vm-driver=virtualbox
```
```
...
Configuring environment for Kubernetes v1.14.2 on Docker 18.09.6
...
```

- Review Kubernetes dashboard by running the following command.

```sh
$ minikube dashboard
```

- Confirm that your kubectl is configured correctly by running the following command.

```sh
$ kubectl version
```
```
Client Version: version.Info{Major:"1", Minor:"14+", GitVersion:"v1.14.10-dispatcher", ...}
Server Version: version.Info{Major:"1", Minor:"14", GitVersion:"v1.14.2", ...}
```

- Confirm that your Helm 3 is configured correctly by running the following command.

```sh
$ helm version
```
```
version.BuildInfo{Version:"v3.0.3", GitCommit:"...", GitTreeState:"clean", GoVersion:"go1.13.6"}
```

## Download YugabyteDB Helm Chart

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

```sh
NAME               	CHART VERSION	APP VERSION	DESCRIPTION
yugabytedb/yugabyte	2.1.4        	2.1.4.0-b5 	YugabyteDB is the high-performance distributed ...```
```

Now you are ready to create a local YugabyteDB cluster.


{{<tip title="Next step" >}}

[Create a local cluster](../../create-local-cluster/kubernetes)

{{< /tip >}}


