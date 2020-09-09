---
title: DocDB sharding layer
headerTitle: DocDB sharding layer
linkTitle: DocDB sharding layer
description: Learn about sharding strategies, hash and range sharding, colocated tables, and table splitting.
image: /images/section_icons/architecture/concepts.png
aliases:
  - /stable/architecture/docdb/sharding
headcontent: Learn about sharding strategies, hash and range sharding, colocated tables, and table splitting.
block_indexing: true
menu:
  stable:
    identifier: architecture-docdb-sharding
    parent: architecture
    weight: 1130
---

{{< note title="Note" >}}

YugabyteDB's sharding architecture is inspired by <a href="https://research.google.com/archive/spanner-osdi2012.pdf">Google Spanner</a>.

{{</note >}}

This section describes how sharding works in DocDB. A distributed SQL database needs to automatically partition the data in a table and distribute it across nodes. This is known as data sharding and it can be achieved through different strategies, each with its own tradeoffs.

Data sharding helps in scalability and geo-distribution by horizontally partitioning data. A SQL table is decomposed into multiple sets of rows according to a specific sharding strategy. Each of these sets of rows is called a shard. These shards are distributed across multiple server nodes (containers, VMs, bare-metal) in a shared-nothing architecture. This ensures that the shards do not get bottlenecked by the compute, storage and networking resources available at a single node. High availability is achieved by replicating each shard across multiple nodes. However, the application interacts with a SQL table as one logical unit and remains agnostic to the physical placement of the shards.

DocDB supports range and hash sharding natively.


<div class="row">
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="sharding/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/concepts/sharding.png" aria-hidden="true" />
        <div class="title">Sharding strategies</div>
      </div>
      <div class="body">
        The supported sharding strategies in DocDB, hash and range sharding.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="tablet-splitting/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/concepts/replication.png" aria-hidden="true" />
        <div class="title">Tablet splitting</div>
      </div>
      <div class="body">
        How tablet splitting works in DocDB. This includes presplitting tablets, manual splitting and automatic splitting.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="colocated-tables/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/linear_scalability.png" aria-hidden="true" />
        <div class="title">Colocated tables</div>
      </div>
      <div class="body">
        Scaling number of relations by colocating data.
      </div>
    </a>
  </div

</div>