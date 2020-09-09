---
title: Batch operations in YSQL
headerTitle: Batch operations
linkTitle: 6. Batch operations
description: Learn how batch operations in YSQL send a set of operations as a single RPC call rather than one by one as individual RPC calls.
aliases:
  - /latest/develop/learn/batch-operations/
menu:
  latest:
    identifier: batch-operations-2-ysql
    parent: learn
    weight: 568
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="{{< relref "./batch-operations-ycql.md" >}}" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

Batch operations is the ability to send a set of operations as one operation (RPC call) to the database instead of sending the operations one by one as individual RPC calls. For use cases requiring high throughput, batch operations can be very efficient since it is possible to reduce the overhead of multiple RPC calls. The larger the batch size, the higher the latency for the entire batch. Although the latency for the entire batch of operations is higher than the latency of any single operation, the throughput of the batch of operations is much higher.

YSQL content coming soon.
