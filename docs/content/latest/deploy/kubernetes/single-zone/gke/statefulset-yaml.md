---
title: Deploy on Google Kubernetes Engine (GKE) using YAML (remote disk)
headerTitle: Google Kubernetes Engine (GKE)
linkTitle: Google Kubernetes Engine (GKE)
description: Deploy a single-zone YugabyteDB cluster on Google Kubernetes Engine (GKE) using YAML (remote disk).
menu:
  latest:
    parent: deploy-kubernetes-sz
    name: Google Kubernetes Engine
    identifier: k8s-gke-2
    weight: 623
aliases:
  - /latest/deploy/kubernetes/gke/statefulset-yaml
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/gke/helm-chart" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/gke/statefulset-yaml" class="nav-link active">
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

- Download and install the [Google Cloud SDK](https://cloud.google.com/sdk/downloads/).

**NOTE:** If you install `gcloud` using a package manager (as opposed to downloading and installing it manually), it does not support some of the commands below.

- Install `kubectl`

After installing the Google Cloud SDK, install the `kubectl` command line tool by running the following command:

```sh
$ gcloud components install kubectl
```

- Configure defaults for gcloud

Set the project ID as `yugabyte`. You can change this as per your need.

```sh
$ gcloud config set project yugabyte
```

Set the default compute zone as `us-west1-b`. You can change this as per your need.

```sh
$ gcloud config set compute/zone us-west1-b
```

## 1. Create a GKE cluster

Create a Kubernetes cluster, if you have not already done so, by running the following command.

```sh
$ gcloud container clusters create yugabyte
```

## 2. Create a YugabyteDB cluster

Create a YugabyteDB cluster by running the following.

```sh
$ kubectl create -f https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/cloud/kubernetes/yugabyte-statefulset.yaml
```

```
service "yb-masters" created
statefulset "yb-master" created
service "yb-tservers" created
statefulset "yb-tserver" created
```

## 3. Check the cluster

You should see the following pods running.

```sh
$ kubectl get pods
```

```
NAME           READY     STATUS    RESTARTS   AGE
yb-master-0    1/1       Running   0          3m
yb-master-1    1/1       Running   0          3m
yb-master-2    1/1       Running   0          3m
yb-tserver-0   1/1       Running   0          3m
yb-tserver-1   1/1       Running   0          3m
yb-tserver-2   1/1       Running   0          3m
```

You can view the persistent volumes.

```sh
$ kubectl get persistentvolumes
```

```
NAME                                       CAPACITY   ACCESS MODES   RECLAIM POLICY   STATUS    CLAIM                          STORAGECLASS   REASON    AGE
pvc-f3301c41-1110-11e8-8231-42010a8a0083   1Gi       RWO            Delete           Bound     default/datadir-yb-master-0    standard                 5m
pvc-f33f29b3-1110-11e8-8231-42010a8a0083   1Gi       RWO            Delete           Bound     default/datadir-yb-master-1    standard                 5m
pvc-f35005b6-1110-11e8-8231-42010a8a0083   1Gi       RWO            Delete           Bound     default/datadir-yb-master-2    standard                 5m
pvc-f36189ab-1110-11e8-8231-42010a8a0083   1Gi       RWO            Delete           Bound     default/datadir-yb-tserver-0   standard                 5m
pvc-f366a4af-1110-11e8-8231-42010a8a0083   1Gi       RWO            Delete           Bound     default/datadir-yb-tserver-1   standard                 5m
pvc-f36d2892-1110-11e8-8231-42010a8a0083   1Gi       RWO            Delete           Bound     default/datadir-yb-tserver-2   standard                 5m
```

You can view all the services by running the following command.

```sh
$ kubectl get services
```

```
NAME          TYPE        CLUSTER-IP   EXTERNAL-IP   PORT(S)                               AGE
kubernetes    ClusterIP   XX.XX.XX.X   <none>        443/TCP                               23m
yb-masters    ClusterIP   None         <none>        7000/TCP,7100/TCP                     17m
yb-tservers   ClusterIP   None         <none>        9000/TCP,9100/TCP,9042/TCP,6379/TCP   14m
```

## 4. Connect to the cluster

You can connect to the YCQL API by running the following.

```sh
$ kubectl exec -it yb-tserver-0 -- ycqlsh yb-tserver-0
```

```
Connected to local cluster at 127.0.0.1:9042.
[cqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh> DESCRIBE KEYSPACES;

system_schema  system_auth  system
```

## 5. Destroy cluster (optional)

Destroy the YugabyteDB cluster you created above by running the following.

```sh
$ kubectl delete -f https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/cloud/kubernetes/yugabyte-statefulset.yaml
```

```
service "yb-masters" deleted
statefulset "yb-master" deleted
service "yb-tservers" deleted
statefulset "yb-tserver" deleted
```

To destroy the persistent volume claims (**you will lose all the data if you do this**), run:

```sh
$ kubectl delete pvc -l app=yb-master
$ kubectl delete pvc -l app=yb-tserver
```

## 6. Destroy the GKE cluster (optional)

To destroy the machines you created for the `gcloud` cluster, run the following command.

```sh
$ gcloud container clusters delete yugabyte
```
