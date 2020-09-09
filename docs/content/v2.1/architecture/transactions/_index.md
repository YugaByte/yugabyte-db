---
title: DocDB transactions layer
headerTitle: DocDB transactions layer
linkTitle: DocDB transactions layer
description: DocDB transactions layer
image: /images/section_icons/architecture/distributed_acid.png
headcontent: DocDB is YugabyteDB's distributed document store responsible for transactions, sharding, replication, and persistence.
block_indexing: true
menu:
  v2.1:
    identifier: architecture-acid-transactions
    parent: architecture
    weight: 1120
---

{{< note title="Note" >}}

YugabyteDB's distributed ACID transaction architecture is inspired by <a href="https://research.google.com/archive/spanner-osdi2012.pdf">Google Spanner</a>.

{{</note >}}

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="transactions-overview/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/distributed_acid.png" aria-hidden="true" />
        <div class="title">Transactions overview</div>
      </div>
      <div class="body">
        An overview of transactions support in YugabyteDB.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="isolation-levels/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/distributed_acid.png" aria-hidden="true" />
        <div class="title">Transaction isolation</div>
      </div>
      <div class="body">
        Understanding supported transaction isolation levels (the "I" in ACID).
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="explicit-locking/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/distributed_acid.png" aria-hidden="true" />
        <div class="title">Explicit row locks</div>
      </div>
      <div class="body">
        Learn about how YugabyteDB uses a combination of optimistic and pessimistic concurrency control to support explicit row locking.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="single-row-transactions/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/distributed_acid.png" aria-hidden="true" />
        <div class="title">Single-row transactions</div>
      </div>
      <div class="body">
        How single row transactions work in YugabyteDB
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="distributed-txns/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/distributed_acid.png" aria-hidden="true" />
        <div class="title">Distributed transactions</div>
      </div>
      <div class="body">
        How distributed (aka multi-shard) transactions work in YugabyteDB
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="transactional-io-path/">
      <div class="head">
        <img class="icon" src="/images/section_icons/architecture/distributed_acid.png" aria-hidden="true" />
        <div class="title">Transactional IO path</div>
      </div>
      <div class="body">
        How reads and writes occur during transactions
      </div>
    </a>
  </div>
</div>
