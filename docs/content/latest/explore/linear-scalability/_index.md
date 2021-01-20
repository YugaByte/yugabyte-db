---
title: Horizontal Scalability
headerTitle: Horizontal Scalability
linkTitle: Horizontal Scalability
description: Horizontal Scalability in YugabyteDB.
headcontent: Horizontal Scalability in YugabyteDB.
aliases:
  - /explore/linear-scalability/
  - /latest/explore/linear-scalability/
  - /latest/explore/cloud-native/linear-scalability/
  - /latest/explore/postgresql/linear-scalability/
  - /latest/explore/linear-scalability-macos/
  - /latest/explore/linear-scalability/macos/
  - /latest/explore/linear-scalability/linux/
  - /latest/explore/linear-scalability/docker/
  - /latest/explore/linear-scalability/kubernetes/
  - /latest/explore/auto-sharding/macos/
  - /latest/explore/auto-sharding/linux/
  - /latest/explore/auto-sharding/docker/
  - /latest/explore/auto-sharding/kubernetes/
image: /images/section_icons/explore/linear_scalability.png
menu:
  latest:
    identifier: explore-scalability
    parent: explore
    weight: 220
---

A YugabyteDB cluster can be scaled horizontally (to increase the aggregate vCPUs, memory and disk in the database cluster) by dynamically adding nodes to a running cluster, or by increasing the number of pods in the `yb-tserver` StatefulSet in the case of Kubernetes deployments.


A YugabyteDB cluster is scaled out so that it can handle:
* More transactions per second
* Higher number of concurrent client connections
* Larger datasets

{{< note title="Note" >}}

A YugabyteDB cluster can be dynamically *scaled out* by adding nodes (or increasing the number of pods in the case of Kubernetes). It can also be *scaled in* dynamically by draining all the data from existing cluster nodes (or pods), and subsequently removing them from the cluster.

{{</note >}}

### How scalability works

Every table in YugabyteDB is transparently sharded using the primary key of the table, each of these shards are called *tablets*. Each tablet consists of a set of rows in a table. In YugabyteDB, tables are automatically into tablets. This is done at time of creating the table if possible. Tablets can also be split dynamically.

The table below summarizes the support for scalability and sharding across YSQL and YCQL APIs.

| <span style="font-size:20px;">Property</span> | <span style="font-size:20px;">YSQL</span> | <span style="font-size:20px;">YCQL</span> | <span style="font-size:20px;">Comments</span> |
|--------------------------------------------------|-------------|----------|----------|
| <span style="font-size:16px;">[Scale transactions per sec](scaling-transactions/)</span> | <span style="font-size:16px;">Yes</span> | <span style="font-size:16px;">Yes</span> | Scale out the cluster to handle a higher number of concurrent transactions per sec. |
| <span style="font-size:16px;">[Data distribution support](sharding-data/)</span> | <span style="font-size:16px;">Hash sharding, <br/>  Range sharding</span>  | <span style="font-size:16px;">Hash sharding</span> | Sharding is used to distributed data across nodes of clusters. <br/> Tables can be pre-split at creation time, and dynamically split at runtime. |

The various features are explained in the sections below.

<div class="row">

   <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="scaling-transactions">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/linear_scalability.png" aria-hidden="true" />
        <div class="title">Scaling transactions per second</div>
      </div>
      <div class="body">
        Scale out the cluster to handle a higher number of concurrent transactions per sec.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="sharding-data/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/auto_sharding.png" aria-hidden="true" />
        <div class="title">Data distribution across nodes</div>
      </div>
      <div class="body">
        Automatic data distribution across nodes of the cluster using transparent sharding of tables.
      </div>
    </a>
  </div>

</div>
