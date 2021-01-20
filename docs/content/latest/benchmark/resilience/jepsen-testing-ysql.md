---
title: Jepsen testing of YSQL
headerTitle: Jepsen testing
linkTitle: Jepsen testing
description: Learn the results from Jepsen testing of YSQL.
image: /images/section_icons/explore/high_performance.png
menu:
  latest:
    identifier: jepsen-testing-1-ysql
    parent: resilience
    weight: 13
showAsideToc: true
isTocNested: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/benchmark/resilience/jepsen-testing-ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>

  <li >
    <a href="/latest/benchmark/resilience/jepsen-testing-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>

</ul>

The Yugabyte SQL API (YSQL) passed [Jepsen testing, performed by Kyle Kingsbury](https://jepsen.io/services), on YugabyteDB v1.3.1.2, with the exception of transactional DDL support, which virtually no other distributed SQL database supports. The impact of this issue is minor as it is limited to cases where DML happens before DDL has fully finished. [GitHub issue #2021](https://github.com/yugabyte/yugabyte-db/issues/2021) has already been fixed to take care of this issue.

YugabyteDB supports serializable and snapshot isolation for transactions. The previous v1.2 release shipped with the Jepsen verification of YCQL, the Cassandra-inspired, semi-relational Yugabyte YCQL API. Because the YugabyteDB underlying distributed document store (DocDB), is common across both the YCQL and YSQL APIs, there was no surprise that YSQL relatively easily passed almost all of the official Jepsen safety tests (except for transactional DDL support).

The primary focus of this Jepsen testing was to test the new serializable isolation level for distributed transactions, where isolation stands for the “I” in ACID. As a fully-relational SQL API, YSQL supports both serializable and snapshot isolation while the semi-relational YCQL API supports only the snapshot isolation level.

By passing this Jepsen testing, YugabyteDB has the distinction of being the first database to pass Jepsen testing for two separate APIs. For details, see the [official Jepsen tests report by Kyle Kingsbury](https://jepsen.io/analyses/yugabyte-db-1.3.1). Below is a summary and discussion of the highlights from the report.

## Accelerated failure testing

Jepsen tests accelerate the failures that would be observed in production systems by constantly and frequently introducing faults. The [Jepsen testing methodology](https://jepsen.io/analyses) notes that bugs reproduced in Jepsen are observable in production and are not theoretical. Jepsen tests construct random operations, apply them to the system, and construct a concurrent history of the results. That history is then checked against a model to establish its correctness.

The report outlines a variety of faults that are introduced while performing operations on a YugabyteDB cluster, including:

- Crashes of the various processes (`yb-master` and `yb-tserver`)
- Network partitions (single-node, majority-minority and non-transitive)
- Process pauses
- Instantaneous and stroboscopic changes to clocks, up to hundreds of seconds
- Combinations of the above events

Jepsen tests have proven to be highly effective in detecting issues. Because correctness is paramount to YugabyteDB as an OLTP database, Yugabyte performs Jepsen tests as part of the nightly CI/CD pipeline on the YugabyteDB release builds.

## Safety issues identified

The array of tests performed by Kyle Kingsbury uncovered two safety issues:

- G2-item (anti-dependency) cycles in transactions
- Improperly initialized DEFAULT now() columns

Below is a brief summary about both of these issues.

### Item cycles in transactions

The append test detected a serializability violation under a rare situation that was only discovered after almost 100 hours of testing by inducing `yb-master` process crashes. The `yb-master` process is responsible for storing system metadata (for example, shard-to-node mappings) and coordinating system-wide operations (for example, automatic load balancing, table creation, and schema changes). It does not handle queries issued by application clients. Even though the scenario under which the failure occurs is rather obscure, Yugabyte appreciates the thoroughness of testing performed by Kyle Kingsbury.

Here is a summary of the relevant sequence of events under which the bug is detected:

1. Say that a new master gets elected as Raft leader (the "master leader").
2. Immediately after the master is elected as the new Raft leader, the master leader has an empty capabilities set for the tablet servers in the cluster. The capabilities set describes the set of features supported by the tablet servers, and is used instead of version numbers, in order to preserve backward compatibility for rolling upgrades.
3. Tablet servers begin sending heartbeats to the master leader. This happens quickly (less than 500 milliseconds with default settings).
4. As soon as the master leader receives heartbeats from the tablet servers, the capabilities set gets updated.

The serializability violation occurs if the YSQL layer queries the master leader and receives an empty tablet server capabilities set in the window between steps 2 and 3 above. This empty set causes any subsequent transaction RPC request to include a read time field. Normally, this read time should be ignored by the tablet server in the case of serializable transactions (it is an optimization that is valid only for snapshot isolation levels). If the field was set, however, the serializable transaction ends up using the read timestamp, and eventually results in a serializability violation. To learn more about the details of this issue, see [GitHub issue #2125](https://github.com/YugaByte/yugabyte-db/issues/2125), which was promptly resolved by [this commit](https://github.com/mbautin/yugabyte-db/commit/3e093529482e048664efd729f6ab820a2b719cf9).

### Improperly initialized default now()

YSQL does not support transactional DDL statements yet ([GitHub issue #1404](https://github.com/YugaByte/yugabyte-db/issues/1404)), meaning that the multiple steps required to perform an operation, such as creating a table with indexes, are not executed in an atomic manner. This test failure, where columns with a default value of `now()` would sometimes be initialized to `NULL`, is a symptom of that underlying issue. Let’s dive right into how this happens.

The underlying table was defined as shown below.

```plpgsql
CREATE TABLE foo (
  k TIMESTAMP DEFAULT now(),
  v INT
) IF NOT EXISTS;
```

The default `TIMESTAMP` column value is `now()`, a traditional PostgreSQL equivalent to `transaction_timestamp()`. For details, see [Current Date/Time](https://www.postgresql.org/docs/11/functions-datetime.html#FUNCTIONS-DATETIME-CURRENT) in the PostgreSQL documentation.

In order to create the table `foo` above in YSQL, a number of discrete steps need to be performed. Here are some of the steps relevant to this issue:

1. Write the table schema into the YSQL system catalog without the `DEFAULT` column value.
2. Add the `pg_class` and `pg_attribute` entries.
3. Modify the entries to set the default column value.

Assume the steps above are being performed by a client. The table becomes operational after step 2 and an independent client can successfully insert data before step 3 is complete, when the default column value is set. Any inserts that occur before step 3 see `NULL` values instead of `now()` for the column `k`. In summary, this issue turned out to be not related to the core design or implementation of the YugabyteDB transaction layer that supports serializable and snapshot isolation levels, but occurs because the implementation of the `CREATE TABLE` sequence is not yet atomic. A simple, short-term workaround is to wait for the table creation to succeed before starting the workload against the table.

## Kudos from the Jepsen report

Yugabyte works diligently to ensure that distributed transactions in YugabyteDB are robust to all types of failures, including clock skews. Yugabyte is proud to see this recognized in the Jepsen report. Below are a couple of observations worth calling out.

### Robust to clock skews

From the Jepsen report: "Whatever the case, this is a good thing for operators: nobody wants to worry about clock safety unless they have to, and YugabyteDB appears to be mostly robust to clock skew. Keep in mind that we cannot robustly test YugabyteDB’s use of `CLOCK_MONOTONIC_RAW` for leader leases, but we suspect skew there is less of an issue than `CLOCK_REALTIME` synchronization."

YugabyteDB relies on the `CLOCK_MONOTONIC_RAW` for leader leases instead of `CLOCK_REALTIME`. In simple terms, this means that YugabyteDB uses the underlying hardware clock and not the clock that displays the current time on a node. As a result, YugabyteDB is resistant to clock skews.

### Rare occurrence of causal reversal

From the Jepsen report: "YugabyteDB was relatively robust in our transactional tests. Although it claims to provide only serializability, and theoretically allows non-linearizable phenomena like causal reverse, these anomalies were rare in our testing."

*Causal reverse* is the phenomenon where the order of writes to independent keys is reversed in the serial order. For example, let’s say there is an update to key `X` in a user’s application which results in an update to an unrelated key Y. Clearly, from the application’s point of view, `X` precedes `Y` in time. However, from the point of view of YugabyteDB, these operations are unrelated to each other and act on non-overlapping set of keys, and hence their order could get reversed in time. Causal reverse in itself is not a violation of serializable isolation, and applications typically do not rely on the ordering of unrelated operations. That said, it is encouraging to see that this phenomenon was rare to reproduce experimentally, as this is another indicator of the fact that YugabyteDB is somewhat robust to clock skew.
