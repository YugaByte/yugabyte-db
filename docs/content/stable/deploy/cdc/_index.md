---
title: Change data capture (CDC)
headerTitle: Change data capture (CDC)
linkTitle: Change data capture (CDC)
description: Use change data capture (CDC) to asynchronously stream data changes.
headcontent: Asynchronously stream data changes from YugabyteDB to external systems.
image: /images/section_icons/sample-data/s_s1-sampledata-3x.png
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
block_indexing: true
menu:
  stable:
    identifier: cdc
    parent: deploy
    weight: 632
aliases:
  - /stable/deploy/cdc/use-cdc/
---

<a href="../../architecture/cdc-architecture">Change data capture (CDC)</a> can be used to asynchronously stream data changes from a YugabyteDB cluster to external systems like message queues and OLAP warehouses. The data changes in YugabyteDB are detected, captured, and then output to the specified target. 

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="cdc-to-kafka/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/json_documents.png" aria-hidden="true">
        <div class="title">CDC to Apache Kafka</div>
      </div>
      <div class="body">
        Use CDC to stream data changes in YugabyteDB to Apache Kafka.
      </div>
    </a>
  </div>

</div>
