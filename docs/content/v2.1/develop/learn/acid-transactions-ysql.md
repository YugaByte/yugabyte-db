---
title: ACID transactions in YSQL
headerTitle: ACID transactions
linkTitle: 4. ACID transactions
description: Learn how ACID transactions work in YSQL on YugabyteDB.
block_indexing: true
menu:
  v2.1:
    identifier: acid-transactions-2-ysql
    parent: learn
    weight: 566
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/develop/learn/acid-transactions-ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/develop/learn/acid-transactions-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

A transaction is a sequence of operations performed as a single logical unit of work. A transaction has four key properties - **Atomicity**, **Consistency**, **Isolation** and **Durability** - commonly abbreviated as ACID.

- **Atomicity** All the work in a transaction is treated as a single atomic unit - either all of it is performed or none of it is.

- **Consistency** A completed transaction leaves the database in a consistent internal state. This can either be all the operations in the transactions succeeding or none of them succeeding.

- **Isolation** This property determines how/when changes made by one transaction become visible to the other. For example, a *serializable* isolation level guarantees that two concurrent transactions appear as if one executed after the other (i.e. as if they occur in a completely isolated fashion). YugabyteDB supports both *Snapshot Isolation*, and *Serializable* isolation levels. Read more about the different [levels of isolation](../../../architecture/transactions/isolation-levels/).

- **Durability** The results of the transaction are permanently stored in the system. The modifications must persist even in the instance of power loss or system failures.

YSQL content coming soon.
