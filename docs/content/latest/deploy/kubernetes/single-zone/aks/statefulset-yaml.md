---
title: Deploy on Azure Kubernetes Service (AKS) using StatefulSet YAML
headerTitle: Azure Kubernetes Service (AKS)
linkTitle: Azure Kubernetes Service (AKS)
description: Use StatefulSet YAML to deploy a single-zone Kubernetes cluster on Azure Kubernetes Service (AKS).
menu:
  latest:
    parent: deploy-kubernetes-sz
    name: Azure Kubernetes Service
    identifier: k8s-aks-2
    weight: 624
aliases:
  - /latest/deploy/kubernetes/aks/statefulset-yaml
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/aks/helm-chart" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/aks/statefulset-yaml" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      StatefulSet YAML
    </a>
  </li>
</ul>

## Prerequisites

- Connect to the Azure Cloud Shell. You can connect and get a shell from the browser by navigating to the [Azure bash cloud shell](https://shell.azure.com/bash).

- Register the necessary Azure service providers by running the following:

```sh
az provider register -n Microsoft.Network
az provider register -n Microsoft.Storage
az provider register -n Microsoft.Compute
az provider register -n Microsoft.ContainerService
```

- Configure a default location. Remember to replace `eastus` with an appropriate Azure location (region) of your choice that supports AKS clusters.

```sh
az configure --defaults location=eastus
```

## 1. Create an Azure cluster

- Create an Azure resource

An Azure resource group is a logical group in which Azure resources are deployed and managed. You need to specify a default location or pass the location parameter to create the resource. The resources you create for the AKS cluster will live in this Azure resource.

```sh
$ az group create --name yb-eastus-resource
```

- Create the AKS cluster.

You can create a three-node AKS cluster by running the following command.

```sh
$ az aks create --resource-group yb-eastus-resource --name yb-aks-cluster --node-count 3 --generate-ssh-keys
```

Configure `kubectl` to work with this cluster.

```sh
$ az aks get-credentials --resource-group yb-eastus-resource --name yb-aks-cluster
```

Verify the cluster by running the following command.

```sh
$ kubectl get nodes
```

```
NAME                       STATUS    ROLES     AGE       VERSION
aks-nodepool1-25019584-0   Ready     agent     4h        v1.7.9
aks-nodepool1-25019584-1   Ready     agent     4h        v1.7.9
aks-nodepool1-25019584-2   Ready     agent     4h        v1.7.9
```

## 2. Create a YugabyteDB cluster

Create a YugabyteDB cluster by running the following.

```sh
$ curl -s "https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/cloud/kubernetes/yugabyte-statefulset.yaml" | sed "s/storageClassName: standard/storageClassName: default/g" | kubectl create -f -
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
pvc-849395f7-36f2-11e8-9445-0a58ac1f27f1   1Gi        RWO            Delete           Bound     default/datadir-yb-master-0    default                  12m
pvc-8495d8cd-36f2-11e8-9445-0a58ac1f27f1   1Gi        RWO            Delete           Bound     default/datadir-yb-master-1    default                  12m
pvc-8498b836-36f2-11e8-9445-0a58ac1f27f1   1Gi        RWO            Delete           Bound     default/datadir-yb-master-2    default                  12m
pvc-84abba1a-36f2-11e8-9445-0a58ac1f27f1   1Gi        RWO            Delete           Bound     default/datadir-yb-tserver-0   default                  12m
pvc-84af3484-36f2-11e8-9445-0a58ac1f27f1   1Gi        RWO            Delete           Bound     default/datadir-yb-tserver-1   default                  12m
pvc-84b35d19-36f2-11e8-9445-0a58ac1f27f1   1Gi        RWO            Delete           Bound     default/datadir-yb-tserver-2   default                  12m
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

To open the YCQL shell (`ycqlsh`), run the following command:

```sh
$ kubectl exec -it yb-tserver-0 -- ycqlsh yb-tserver-0
```

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh> DESCRIBE KEYSPACES;

system_schema  system_auth  system
```

## 5. Destroy the YugabyteDB cluster (optional)

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

## 6. Destroy the AKS cluster (optional)

To destroy the resource you created for the AKS cluster, run the following.

```sh
$ az group delete --name yb-eastus-resource
```
