---
title: Multi-Region Deployments
headerTitle: Multi-Region Deployments
linkTitle: Multi-Region Deployments
description: Multi-Region Deployments in YugabyteDB.
headcontent: Multi-Region Deployments in YugabyteDB.
image: /images/section_icons/secure/authorization.png
aliases:
  - /secure/authorization/
menu:
  latest:
    identifier: explore-multi-region-deployments
    parent: explore
    weight: 234
---

YugabyteDB supports a rich set of multi-region deployment topologies. This section explains some of these deployments. The predominant deployments include:

* **Default synchronous** replication across regions
* **Geo-partitioning** to keep data pinned to different geographic locations based on policy
* **xCluster asynchronous replication** for unidirectional and bidirectional replication
* **Read replicas** which internally use asynchronous replication and can only serve reads

The table below summarizes these different multi-region deployments in YugabyteDB along with some of their key characteristics.


|                             | [Default](synchronous-replication-ysql/) | [Geo-partitioning](row-level-geo-partitioning/) | [xCluster](asynchronous-replication-ysql/) | Read replicas
|-----------------------------|---------|------------------|-----------------------------|-----------------------------
|<strong>Replication</strong> | Synchronous | Synchronous  | Asynchronous <br/> *(unidirectional and bidirectional)* | Asynchronous <br/> *(unidirectional only)*
|<strong>Data residency</strong> | All data replicated across regions | Data partitioned across regions. <br/>Partitions replicated inside region. | All data replicated inside region. <br/>Configure per-table cross-region replication. | All data replicated in primary region. <br/>Cluster-wide async replication to read replicas.
| <strong>Consistency</strong> | Transactional | Transactional | Eventual consistency | Eventual consistency
| <strong>Write latency</strong> | High latency | Low latency | Low latency | Low latency
| <strong>Read latency</strong> | High latency | Low latency <br/> *(when queried from nearby geography)* | Low latency | Low latency
| <strong>Schema changes</strong> | Transparently managed | Transparently managed | Manual propagation | Transparently managed
| <strong>RPO</strong> <br/> | No data loss | No data loss <br/> *(partial unavailability possible)* | Some data loss | No data loss

The different deployments are explained in the sections below.

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="synchronous-replication-ysql/">
      <div class="head">
        <img class="icon" src="/images/section_icons/secure/rbac-model.png" aria-hidden="true" />
        <div class="title">Synchronous Replication</div>
      </div>
      <div class="body">
          Achieve synchronous replication across 3 or more regions in YugabyteDB.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="asynchronous-replication-ysql/">
      <div class="head">
        <img class="icon" src="/images/section_icons/secure/rbac-model.png" aria-hidden="true" />
        <div class="title">Asynchronous Replication</div>
      </div>
      <div class="body">
          Achieve asynchronous replication across 2 or more regions in YugabyteDB.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="row-level-geo-partitioning/">
      <div class="head">
        <img class="icon" src="/images/section_icons/secure/rbac-model.png" aria-hidden="true" />
        <div class="title">Row-Level Geo-Partitioning</div>
      </div>
      <div class="body">
          Using row-level geo-partitioning in YugabyteDB.
      </div>
    </a>
  </div>

</div>
