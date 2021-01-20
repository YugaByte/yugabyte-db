---
title: Prometheus Integration
headerTitle: Prometheus Integration
linkTitle: Prometheus Integration 
description: Learn about exporting YugabyteDB metrics and monitoring the cluster with Prometheus.
menu:
  latest:
    identifier: observability-4-kubernetes
    parent: explore-observability
    weight: 240
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/observability/prometheus-integration/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/observability/prometheus-integration/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/explore/observability/prometheus-integration/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>
<!--
  <li >
    <a href="/latest/explore/observability-kubernetes" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
-->
</ul>

You can monitor your local YugabyteDB cluster with a local instance of [Prometheus](https://prometheus.io/), a popular standard for time-series monitoring of cloud native infrastructure. YugabyteDB services and APIs expose metrics in the Prometheus format at the `/prometheus-metrics` endpoint.

For details on the metrics targets for YugabyteDB, see [Monitoring with Prometheus](../../../reference/configuration/default-ports/#monitoring-with-prometheus).

If you haven't installed YugabyteDB yet, do so first by following the [Quick Start](../../../quick-start/install/) guide.

## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ kubectl delete -f yugabyte-statefulset.yaml
```

Start a new local cluster - by default, this will create a three-node universe with a replication factor of `3`.

```sh
$ kubectl apply -f yugabyte-statefulset.yaml
```

## Step 6. Clean up (optional)

Optionally, you can shut down the local cluster created in Step 1.

```sh
$ kubectl delete -f yugabyte-statefulset.yaml
```

Further, to destroy the persistent volume claims (**you will lose all the data if you do this**), run:

```sh
kubectl delete pvc -l app=yb-master
kubectl delete pvc -l app=yb-tserver
```
