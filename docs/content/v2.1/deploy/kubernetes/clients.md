---
title: Connect remote clients to Kubernetes clusters
headerTitle: Connect clients to Kubernetes clusters
linkTitle: Connect clients
description: Connect remote clients to YugabyteDB clusters deployed within Kubernetes.
block_indexing: true
menu:
  v2.1:
    identifier: clients-kubernetes
    parent: deploy-kubernetes
    weight: 626
type: page
isTocNested: true
showAsideToc: true
---

## Introduction

This document describes the different options to connect to a Yugabyte cluster deployed within Kubernetes.

## Prerequisites

You must have set up a Yugabyte cluster according to the [Kubernetes deployment instructions.](../../kubernetes).

## Connecting from within the Kubernetes cluster

An application that is deployed within the Kubernetes cluster should use the Service DNS name `yb-tservers.<namespace>.svc.cluster.local` to discover server endpoints. This DNS entry has multiple `A` records, one for each tserver pod, so that clients can randomize queries across different endpoints.

```sh
$ kubectl --namespace yb-demo get svc/yb-tservers
NAME          TYPE        CLUSTER-IP   EXTERNAL-IP   PORT(S)                                        AGE
yb-tservers   ClusterIP   None         <none>        7100/TCP,9000/TCP,6379/TCP,9042/TCP,5433/TCP   56m
```

Here is an example of a client that uses the YSQL shell ([`ysqlsh`](../../../admin/ysqlsh)) to connect.

```sh
$ kubectl run ysqlsh-client -it --rm  --image yugabytedb/yugabyte-client --command -- ysqlsh -h yb-tservers.yb-demo.svc.cluster.local
yugabyte=# CREATE TABLE demo(id INT PRIMARY KEY);
CREATE TABLE
```

Here is an example of a client that uses the YCQL shell ([`ycqlsh`](../../../admin/cqlsh)) to connect.

```sh
$ kubectl run cqlsh-shell -it --rm  --image yugabytedb/yugabyte-client --command -- cqlsh yb-tservers.yb-demo.svc.cluster.local 9042
ycqlsh> CREATE KEYSPACE demo;
ycqlsh> use demo;
ycqlsh:demo> CREATE TABLE t_demo(id INT PRIMARY KEY);
```

Note that although tables are [internally sharded](../../../architecture/concepts/yb-tserver/) across multiple tserver pods, every tserver pod has the ability to process any query, irrespective of its actual tablet assignment.

## Connecting externally

An application that is deployed outside the Kubernetes cluster should use the external LoadBalancer IP address to connect to the cluster. Connections to the load balancer IP address are randomly routed to one of the tserver pods behind the yb-tservers service.

```sh
$ kubectl get svc -n yb-demo
NAME                 TYPE           CLUSTER-IP      EXTERNAL-IP   PORT(S)                                        AGE
yb-master-ui         LoadBalancer   10.101.142.48   98.138.219.231     7000:32168/TCP                                 43h
yb-masters           ClusterIP      None            <none>        7100/TCP,7000/TCP                              43h
yb-tserver-service   LoadBalancer   10.99.76.181    98.138.219.232     6379:30141/TCP,9042:31059/TCP,5433:30577/TCP   43h
yb-tservers          ClusterIP      None            <none>        7100/TCP,9000/TCP,6379/TCP,9042/TCP,5433/TCP   43h
```

Here is an example of a client that uses the YSQL shell ([`ysqlsh`](../../../admin/ysqlsh) to connect.

```sh
$ docker run yugabytedb/yugabyte-client ysqlsh -h 98.138.219.232
yugabyte=# CREATE TABLE demo(id INT PRIMARY KEY);
CREATE TABLE
```

Here is an example of a client that uses the YCQL shell ([`ycqlsh`](../../../admin/cqlsh)) to connect.

```sh
$ docker run yugabytedb/yugabyte-client ycqlsh 98.138.219.232 9042
ycqlsh> CREATE KEYSPACE demo;
ycqlsh> use demo;
ycqlsh:demo> CREATE TABLE t_demo(id INT PRIMARY KEY);
```

### Master UI dashboard

The master UI dashboard is available at the external IP address exposed by the `yb-master-ui` LoadBalancer service - in this case at `http://98.138.219.231:7000/`.

Another option that does not require an external LoadBalancer is to create a tunnel from the local host to the master web server port on the master pod using [kubectl port-forward](https://kubernetes.io/docs/tasks/access-application-cluster/port-forward-access-application-cluster/).

```sh
$ kubectl port-forward pod/yb-master-0 7000:7000 -n yb-demo
Forwarding from 127.0.0.1:7000 -> 7000
Forwarding from [::1]:7000 -> 7000
```

### Connecting externally to a Minikube cluster

When the Kubernetes cluster is set up using [Minikube](https://kubernetes.io/docs/setup/learning-environment/minikube/), an external IP address is not available by default for the LoadBalancer endpoints. To enable the load balancer IP address, run the command `minikube tunnel`. For details, see [LoadBalancer access](https://minikube.sigs.k8s.io/docs/handbook/accessing/#loadbalancer-access).

```sh
$ minikube tunnel
Status:
    machine: minikube
    pid: 38193
    route: 10.96.0.0/12 -> 192.168.99.100
    minikube: Running
    services: [yb-master-ui, yb-tserver-service]
    errors:
        minikube: no errors
        router: no errors
        loadbalancer emulator: no errors
```
