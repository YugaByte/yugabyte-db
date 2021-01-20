---
title: Open source Kubernetes using YugabyteDB operator
headerTitle: Open source Kubernetes
linkTitle: Open source Kubernetes
description: Deploy a YugabyteDB cluster with a Kubernetes native customer resource.
menu:
  latest:
    parent: deploy-kubernetes-sz
    name: Open Source
    identifier: k8s-oss-2
    weight: 621
aliases:
  - /latest/deploy/kubernetes/yugabyte-operator/
  - /latest/deploy/kubernetes/oss/yugabyte-operator/
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="{{< relref "./helm-chart.md" >}}" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
  <li >
    <a href="{{< relref "./yugabyte-operator.md" >}}" class="nav-link active">
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

Create and manage a YugabyteDB cluster with a Kubernetes native custom resource `ybcluster.yugabyte.com`. The custom resource definition and other necessary specifications can be found in [YugabyteDB operator repository](https://github.com/yugabyte/yugabyte-operator/). This operator currently provides more configuration flags as compared to the Rook operator. The Rook operator, in near future, will get these flags too. See full list of configuration flags [here](#configuration-flags).

## Prerequisites

Clone the [yugabyte-operator](https://github.com/yugabyte/yugabyte-operator) repository on your local computer. Change into the cloned directory and follow instructions below.

## Deploy a YugabyteDB cluster with this operator

To create a YugabyteDB cluster, first you need to register the custom resource that would represent YugabyteDB cluster: `ybclusters.yugabyte.com`.

```sh
kubectl create -f deploy/crds/yugabyte.com_ybclusters_crd.yaml
```

Setup RBAC for operator and create the operator itself. Run the following command, from root of the repository, to do the same.

```sh
kubectl create -f deploy/operator.yaml
```

After a few seconds the operator should be up & running. Verify the operator status by running following command.

```sh
kubectl -n yb-operator get po,deployment
```

Finally create an instance of the custom resource with which the operator would create a YugabyteDB cluster.

```sh
kubectl create -f deploy/crds/yugabyte.com_v1alpha1_ybcluster_cr.yaml
```

Verify that the cluster is up and running with the following command. You should see 3 pods each for YB-Master and YB-TServer services.

```sh
kubectl get po,sts,svc
```

Once the cluster is up and running, you may start the YSQL API and start executing relational queries.

```sh
kubectl exec -it yb-tserver-0 -- ysqlsh -h yb-tserver-0 --echo-queries
```

You can also connect to the YCQL API as shown below.

```sh
kubectl exec -it yb-tserver-0 -- ycqlsh yb-tserver-0
```

## Configuration flags

### Image

Mention YugabyteDB Docker image attributes such as `repository`, `tag` and `pullPolicy` under `image`.

### Replication factor

Specify the required data replication factor. This is a **required** field.

### TLS

Enable TLS encryption for YugabyteDB, if desired. It is disabled by default. You can enable the TLS encryption with 3 flags, explained later. If you have set `enabled` to true, then you need to generate root certificate and key. Specify the two under `rootCA.cert` & `rootCA.key`. Refer to  [TLS encryption docs](../../../../../secure/tls-encryption/server-certificates/) (through the `Generate root configuration` section) for how to generate the certificate and key files.

### YB-Master and YB-TServer

YB-Master and YB-TServer are two essential components of a YugabyteDB cluster. YB-Master is responsible for recording and maintaining system metadata and for administration activities. YB-TServer is responsible for data I/O.
Specify YB-Master and YB-TServer attributes under `master`/`tserver`. The valid attributes are as described below. These two are **required** fields.

#### Replicas

Specify count of pods for `master` & `tserver` under `replicas` field. This is a **required** field.

#### Network ports

Control network configuration for Master & TServer, each of which support only a selected port attributes. Below table depicts the supported port attributes.
Note that these are **optional** fields, except `tserver.tserverUIPort`, hence below table also mentions default values for each port. Default network configuration will be used, if any or all of the acceptable fields are absent.

A ClusterIP service will be created when `tserver.tserverUIPort` port is specified. If it is not specified, only StatefulSet & headless service will be created for TServer. ClusterIP service creation will be skipped. Whereas for Master, all 3 kubernetes objects will always be created.

If `master.enableLoadBalancer` is set to `true`, then master UI service will be of type `LoadBalancer`. TServer UI service will be of type `LoadBalancer`, if `tserver.tserverUIPort` is specified and `tserver.enableLoadBalancer` is set to `true`. `tserver.enableLoadBalancer` will be ignored if `tserver.tserverUIPort` is not specified.

Table depicting acceptable port names, applicable component (Master/TServer) and port default values:

| Attribute      | Component | Default Value |
| -------------- | --------- | ------------- |
| masterUIPort   | Master    | 7000          |
| masterRPCPort  | Master    | 7100          |
| tserverUIPort  | TServer   | NA            |
| tserverRPCPort | TServer   | 9100          |
| ycqlPort       | TServer   | 9042          |
| yedisPort      | TServer   | 6379          |
| ysqlPort       | TServer   | 5433          |

#### podManagementPolicy

Specify pod management policy for Statefulsets created as part of YugabyteDB cluster. Valid values are `Parallel` & `OrderedReady`, `Parallel` being the default value.

#### storage

Specify storage configurations viz. Storage `count`, `size` & `storageClass` of volumes. Typically, 1 volume per Master instance is sufficient, hence Master has a default storage count of `1`. If storage class isn't specified, it will be defaulted to `standard`. Make sure kubernetes admin has defined `standard` storage class, before leaving this field out.

#### resources

Specify resource `requests` and `limits` under `resources` attribute. The resources to be specified are `cpu` and `memory`. The `resource` property in itself is optional and will not be applied to created `StatefulSets`, if omitted. You may also choose to specify either `resource.requests` or `resource.limits`, or both.

#### flags

Specify flags for additional control of the YugabyteDB cluster. For available, see [YB-Master flags](../../../../../reference/configuration/yb-master/#flags) and [YB-TServer flags](../../../../../reference/configuration/yb-tserver/#flags).

If you have enabled TLS encryption, then you can set:

- `use_node_to_node_encryption` flag to enable node-to-node encryption
- `allow_insecure_connections` flag to specify if insecure connections are allowed when TLS is enabled
- `use_client_to_server_encryption` flag to enable client-to-node encryption
