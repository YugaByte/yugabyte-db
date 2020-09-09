---
title: DocDB replication layer
headerTitle: DocDB replication layer
linkTitle: DocDB replication layer
description: Learn how synchronous and asynchronous replication work in DocDB, including advanced features like xCluster replication and read replicas.
image: /images/section_icons/architecture/concepts.png
aliases:
  - /stable/architecture/docdb/replication/
headcontent: Learn how synchronous and asynchronous replication work in DocDB.
block_indexing: true
menu:
  stable:
    identifier: architecture-docdb-replication
    parent: architecture
    weight: 1135
---

{{< note title="Note" >}}

* YugabyteDB's synchronous replication architecture is inspired by <a href="https://research.google.com/archive/spanner-osdi2012.pdf">Google Spanner</a>. 
* YugabyteDB asynchronous replication architecture is inspired by RDBMS databases such as Oracle, MySQL and PostgreSQL.

{{</note >}}

This section describes how replication works in DocDB. The data in a DocDB table is split into tablets. By default, each tablet is synchronously replicated using the Raft algorithm across various nodes or fault domains (such as availability zones/racks/regions/cloud providers). 

There are other advanced replication features in YugabyteDB. These include two forms of asynchronous replication of data:
* **xCluster replication** Data is asynchronously replicated between different YugabyteDB clusters - both unidirectional replication (master-slave) or  bidirectional replication across two clusters.
* **Read replicas** The in-cluster asynchronous replicas are called read replicas.


<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="replication/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/concepts/replication.png" aria-hidden="true" />
        <div class="title">Default replication</div>
      </div>
      <div class="body">
        In-cluster synchronous replication with Raft consensus.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="async-replication/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/concepts/replication.png" aria-hidden="true" />
        <div class="title">xCluster replication</div>
      </div>
      <div class="body">
        Cross-cluster asynchronous replication of data.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="read-replicas/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/concepts/replication.png" aria-hidden="true" />
        <div class="title">Read replicas</div>
      </div>
      <div class="body">
        In-cluster asynchronous replicas to enable reading data that is a bit stale with lower read latencies.
      </div>
    </a>
  </div>


</div>