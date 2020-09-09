---
title: YugabyteDB query layer (YQL)
headerTitle: Query layer
linkTitle: Query layer
description: Learn how YugabyteDB's extensible query layer implements YSQL and YCQL.
image: /images/section_icons/index/api.png
headcontent: 
aliases:
  - /architecture/query-layer/
  - /stable/architecture/concepts/yql/
block_indexing: true
menu:
  stable:
    identifier: architecture-query-layer
    parent: architecture
    weight: 1110
---

{{< note title="Note" >}}

YugabyteDB has an extensible query layer. It currently implements the following APIs:

* **YSQL**, a distributed SQL API wire compatible with PostgreSQL
* **YCQL**, a semi-relational API built for high performance and massive scale, with its roots in Cassandra Query Language

{{</note >}}

<div class="row">

 <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="overview/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/concepts/query_layer.png" aria-hidden="true" />
        <div class="title">Overview</div>
      </div>
      <div class="body">
        Query layer responsible for language-specific query compilation, execution and optimization.
      </div>
    </a>
  </div>

</div>
