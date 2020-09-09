---
title: YEDIS
linkTitle: YEDIS
headerTitle: Yugabyte Dictionary Service (YEDIS)
description: The YEDIS API allows YugabyteDB to function as a clustered, auto-sharded, globally distributed and persistent key-value database that is compatible with the Redis commands library.
headcontent: 
image: /images/section_icons/api/yedis.png
type: page
section: MISC
block_indexing: true
menu:
  v2.1:
    identifier: yedis
    weight: 2900
---

The YEDIS API allows YugabyteDB to function as a clustered, auto-sharded, globally distributed and persistent key-value database that is compatible with the Redis commands library. A Redis client can connect, send requests, and receive results from this API.

{{< note title="Note" >}}
While YEDIS supports many Redis data types (such as string, hash, set, sorted set and a new Timeseries type) and commands, there are some notable exceptions at present.

* Only a subset of sorted set commands (ZCARD, ZADD, ZRANGEBYSCORE, ZREM, ZRANGE, ZREVRANGE, ZSCORE) have been implemented. Several commands like ZCOUNT, ZREVRANGEBYSCORE, ZRANK, ZREVRANK are not yet implemented.
* List, Bitmaps, HyperLogLogs, GeoSpatial types/commands are not yet implemented.

<b>
In the near-term, YugabyteDB is not actively working on new feature or driver enhancements to the YEDIS API other than bug fixes and stability improvements. Current focus is on YSQL (Postgres-compatible distributed SQL API) and YCQL (a flexible-schema API with Cassandra QL roots).
</b>

For key-value workloads that need persistence, elasticity and fault-tolerance, YCQL (with features
like keyspaces, tables, role-based access control and more) is often a great fit, especially if the
application is new rather than an existing one already written in Redis. The YCQL drivers are also
more clustering aware in terms of routing the request directly to the node which hosts the row/key,
and hence YCQL even performs marginally better than YEDIS for equivalent scenarios. In general, our
new feature development (support for data types, built-ins, TLS, backups and more), correctness
testing (using e.g., Jepsen) and performance work is in the YSQL and YCQL areas.  {{< /note >}}


<div class="row">
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="quick-start/">
      <div class="head">
        <img class="icon" src="/images/section_icons/index/quick_start.png" aria-hidden="true" />
        <div class="title">Quick Start</div>
      </div>
      <div class="body">
          Get started.
      </div>
    </a>
  </div>
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="develop/">
      <div class="head">
        <img class="icon" src="/images/section_icons/index/develop.png" aria-hidden="true" />
        <div class="title">Develop</div>
      </div>
      <div class="body">
          Develop apps.
      </div>
    </a>
  </div>
  <!--
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="deploy/">
      <div class="head">
        <img class="icon" src="/images/section_icons/index/deploy.png" aria-hidden="true" />
        <div class="title">Deploy</div>
      </div>
      <div class="body">
         Deploy on the infrastructure of your choice.
      </div>
    </a>
  </div>
  -->
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="api/">
      <div class="head">
        <img class="icon" src="/images/section_icons/index/api.png" aria-hidden="true" />
        <div class="title">API reference</div>
      </div>
      <div class="body">
         Complete API reference.
      </div>
    </a>
  </div>
</div>
