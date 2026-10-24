---
title: CREATE INDEX statement [YSQL]
headerTitle: CREATE INDEX
linkTitle: CREATE INDEX
description: Use the CREATE INDEX statement to create an index on the specified columns of the specified table.
menu:
  stable_api:
    identifier: ddl_create_index
    parent: statements
type: docs
---

## Synopsis

Use the CREATE INDEX statement to create an index on the specified columns of the specified table. Indexes are primarily used to improve query performance.

In YugabyteDB, indexes are sharded - they are split into tablets and distributed across the different nodes in the cluster, just like regular tables. Index sharding is based on the primary key of the index and is independent of how the main table is sharded and distributed, except for primary key indexes, which are implemented in the main table itself.

## Syntax

{{%ebnf%}}
  create_index,
  index_elem
{{%/ebnf%}}

## Semantics

### Concurrent index creation

Index creation in YugabyteDB can happen CONCURRENTLY or NONCONCURRENTLY. If you specify neither keyword, the index is built concurrently. In certain cases, though, concurrent index creation is not possible and the default mode is nonconcurrent index creation. Some of the common cases are:

- The statement runs inside a transaction block. This includes schema migration tools that wrap each migration in a transaction. The only indication of the switch is the following client notice:

    ```output
    NOTICE:  making create index for table "<table>" nonconcurrent
    DETAIL:  Create index in transaction block cannot be concurrent.
    HINT:  Consider running it outside of a transaction block. See https://github.com/yugabyte/yugabyte-db/issues/6240.
    ```

- The table is partitioned. No notice is issued. See [Partitioned indexes](#partitioned-indexes) for how to build indexes on partitioned tables concurrently.
- The table is temporary.

A nonconcurrent build does not perform online index backfill. Unless [table-level locks](../../../../../explore/transactions/explicit-locking/#table-level-locks) are enabled, concurrent writes during nonconcurrent index creation can result in an inconsistent index. When table-level locks are enabled, those writes wait until the nonconcurrent CREATE INDEX completes instead.

Specify CONCURRENTLY explicitly whenever possible. With the explicit keyword, a statement that cannot be built concurrently (inside a transaction block or on a partitioned table) fails with an error instead of silently switching to a nonconcurrent build.

Concurrent index creation allows data to be modified in the main table while the index is being built. It is implemented by an online index backfill process, which is a combination of a distributed index backfill process that works on existing data using parallel workers, and an online component that mirrors newer changes to main table rows into the index. Nonconcurrent index builds are not safe to perform while there are ongoing changes to the main table, unless [table-level locks](../../../../../explore/transactions/explicit-locking/#table-level-locks) are enabled. When table-level locks are enabled, concurrent writers are blocked until the index creation completes, to ensure safety. The following table summarizes the differences in these two modes.

| Condition | Concurrent | Nonconcurrent |
| :-------- | :--------- | :------------ |
| Safe to do other DMLs during CREATE INDEX? | yes | no (unless [table-level locks](../../../../../explore/transactions/explicit-locking/#table-level-locks) are enabled) |
| Keeps other transactions alive during CREATE INDEX? | mostly | no (unless [table-level locks](../../../../../explore/transactions/explicit-locking/#table-level-locks) are enabled) |
| Parallelizes index loading? | yes | no |

{{< note title="Note" >}}

For more details on how online index backfill works, refer to [Online Index Backfill](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/online-index-backfill.md). Flags controlling the speed of online index backfill are described in [Index backfill flags](../../../../../reference/configuration/yb-tserver/#index-backfill-flags).

{{< /note >}}

### Colocation

If the table is colocated, its index is also colocated; if the table is not colocated, its index is also not colocated.

### Partitioned indexes

Creating an index on a partitioned table automatically creates a corresponding index for every partition in the default tablespace. It's also possible to create an index on each partition individually, which you should do in the following cases:

- The indexes need to be created [CONCURRENTLY](#concurrently): concurrent builds for indexes on partitioned tables aren't supported.
- [Row-level geo-partitioning](../../../../../explore/multi-region-deployments/row-level-geo-partitioning/) is being used and you want to create the index separately on each partition to customize the tablespace in which each index is created.

#### Creating indexes concurrently on partitioned tables

Begin by creating the index on the (parent) partitioned table using the [ONLY](#only) keyword.
The index is created in an INVALID state on only the (parent) partitioned table.
To make the index valid, corresponding indexes have to be created on each of the existing partitions and attached to the parent index using `ALTER INDEX parent_index ... ATTACH PARTITION child_index`.
For example:

```sql
CREATE TABLE parent_partition(c1 int, c2 int) PARTITION BY RANGE (c1);
CREATE TABLE child_part_1 PARTITION OF parent_partition FOR VALUES FROM (0) to (100);
CREATE TABLE child_part_2 PARTITION OF parent_partition FOR VALUES FROM (101) to (200);

CREATE INDEX parent_index ON ONLY parent_partition (c1, c2);

\d parent_partition
```

```output
          Table "public.parent_partition"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 c1     | integer |           |          |
 c2     | integer |           |          |
Partition key: RANGE (c1)
Indexes:
    "parent_index" lsm (c1 HASH, c2 ASC) INVALID
Number of partitions: 2 (Use \d+ to list them.)
```

```sql
CREATE INDEX child_part_1_index ON child_part_1 (c1, c2);
CREATE INDEX child_part_2_index ON child_part_2 (c1, c2);
ALTER INDEX parent_index ATTACH PARTITION child_part_1_index;
ALTER INDEX parent_index ATTACH PARTITION child_part_2_index;

\d parent_partition
```

```output
          Table "public.parent_partition"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 c1     | integer |           |          |
 c2     | integer |           |          |
Partition key: RANGE (c1)
Indexes:
    "parent_index" lsm (c1 HASH, c2 ASC)
Number of partitions: 2 (Use \d+ to list them.)
```

Once all partitions successfully create their indexes, the partitioned index should automatically be promoted out of the INVALID state.

#### Recreating unique index constraints concurrently on partitioned tables

Suppose you have a partitioned table where the parent has a unique constraint.
To recreate indexes, you should avoid directly creating a replacement index on the (parent) partitioned table if it is concurrently taking writes because partitioned tables cannot use [CONCURRENTLY](#concurrently).
Furthermore, if you only want to recreate a subset of the partitions' indexes, recreating the (parent) partitioned index is wasteful.
Follow these steps to recreate a partition index online:

```plpgsql
-- Set up example
CREATE TABLE parent (i int UNIQUE) PARTITION BY RANGE (i);
CREATE TABLE child0 PARTITION OF parent FOR VALUES FROM (0) TO (100000);
CREATE TABLE child1 PARTITION OF parent FOR VALUES FROM (100000) TO (200000);
CREATE TABLE child2 PARTITION OF parent FOR VALUES FROM (200000) TO (300000);
INSERT INTO parent VALUES (generate_series(0, 299999));

-- 1. Build standalone unique index (slow, but non-blocking)
CREATE UNIQUE INDEX CONCURRENTLY child0_i_unique ON child0 (i);
-- 2. Add CHECK constraint matching partition bounds (fast: no scan)
ALTER TABLE child0 ADD CONSTRAINT child0_partition_check
  CHECK (i IS NOT NULL AND i >= 0 AND i < 100000) NOT VALID;
-- 3. Validate the CHECK (slow, but non-blocking on parent)
ALTER TABLE child0 VALIDATE CONSTRAINT child0_partition_check;

BEGIN;
-- 4. Lock the parent to prevent queries from missing the partition's data
--    while it is detached.
LOCK TABLE parent IN ACCESS EXCLUSIVE MODE;
-- 5. Detach (fast)
ALTER TABLE parent DETACH PARTITION child0;
-- 6. Drop the old unique constraint (fast: drops its backing index)
ALTER TABLE child0 DROP CONSTRAINT child0_i_key;
-- 7. Promote standalone index to a constraint (fast: no rebuild)
ALTER TABLE child0 ADD CONSTRAINT child0_i_key UNIQUE USING INDEX child0_i_unique;
-- 8. Reattach (fast: CHECK and UNIQUE already satisfy parent)
ALTER TABLE parent ATTACH PARTITION child0 FOR VALUES FROM (0) TO (100000);
COMMIT;

-- 9. Drop temporary CHECK constraint
ALTER TABLE child0 DROP CONSTRAINT child0_partition_check;
```

Repeat steps 1–9 for `child1`, `child2`, and any other partitions as needed.

{{< note title="Note" >}}
Step 4 and the surrounding `BEGIN`/`COMMIT` block rely on two features:

- {{<tags/feature/ea idea="1114">}}[Object locking](../../../../../architecture/transactions/concurrency-control/#table-level-locks), for the `LOCK` itself: set the YB-TServer flag `enable_object_locking_for_table_locks=true`.
- {{<tags/feature/ea idea="1677">}}[Transactional DDL](../../../../../architecture/transactions/transactional-ddl/), to run the `BEGIN`/`COMMIT` block: set the YB-TServer flag `ysql_yb_ddl_transaction_block_enabled=true`. Object locking depends on this flag as well.

Do not enable these flags on a cluster that uses CDC. Transactional DDL currently doesn't support CDC. See [Limitations](../../../../../architecture/transactions/transactional-ddl/#limitations).

It only serves to hold a lock on the parent so that concurrent reads and writes don't miss the partition's data while it is detached.
If there are no reads or writes against the parent table during the detach, omit Step 4 and run Steps 5 to 8 as individual statements instead.
{{< /note >}}

### UNIQUE

Enforce that duplicate values in a table are not allowed.

### CONCURRENTLY

Enable the use of online index backfill (see [Concurrent index creation](#concurrent-index-creation) for details), with some restrictions:

- When creating an index on a temporary table, online schema migration is disabled.
- CREATE INDEX CONCURRENTLY is not supported for partitioned tables.
- CREATE INDEX CONCURRENTLY is not supported inside a transaction block. A CREATE INDEX without the CONCURRENTLY keyword inside a transaction block doesn't fail; it is converted to a nonconcurrent build (see [Concurrent index creation](#concurrent-index-creation)).

### NONCONCURRENTLY

Disable online index backfill (see [Semantics](#semantics) for details).

### ONLY

Indicates not to recurse creating indexes on partitions, if the table is partitioned. The default is to recurse.

### *access_method_name*

The name of the index access method. By default, `lsm` is used for YugabyteDB tables and `btree` is used otherwise (for example, temporary tables).

[GIN indexes](../../../../../explore/ysql-language-features/indexes-constraints/gin/) can be created in YugabyteDB by using the `ybgin` access method.

### INCLUDE clause

Specify a list of columns which will be included in the index as non-key columns.

### TABLESPACE clause

Specify the name of the [tablespace](../ddl_create_tablespace/) that describes the placement configuration for this index. By default, indexes are placed in the `pg_default` tablespace, which spreads the tablets of the index evenly across the cluster.

### WHERE clause

A [partial index](#partial-indexes) is an index that is built on a subset of a table and includes only rows that satisfy the condition specified in the WHERE clause.

It can be used to exclude NULL or common values from the index, or include just the rows of interest.

This speeds up any writes to the table because rows containing the common column values don't need to be indexed.

It also reduces the size of the index, thereby improving the speed for read queries that use the index.

#### *name*

Specify the name of the index to be created.

#### *table_name*

Specify the name of the table to be indexed.

#### *index_elem*

#### *column_name*

Specify the name of a column of the table.

#### *expression*

Specify one or more columns of the table and must be surrounded by parentheses.

- `HASH` - Use hash of the column. This is the default option for the first column and is used to shard the index table.
- `ASC` — Sort in ascending order. This is the default option for second and subsequent columns of the index.
- `DESC` — Sort in descending order.
- `NULLS FIRST` - Specifies that nulls sort before non-nulls. This is the default when DESC is specified.
- `NULLS LAST` - Specifies that nulls sort after non-nulls. This is the default when DESC is not specified.

### NULLS NOT DISTINCT

When creating an unique index, by default, NULL values are treated as distinct entries (and not equal), allowing multiple nulls in the column. The NULLS NOT DISTINCT option modifies this and causes the index to treat nulls equivalently. With this option, you can enforce that only one NULL value is permitted, aligning with use cases where NULL should represent an absence of value rather than a unique entity.

### SPLIT INTO

For hash-sharded indexes, you can use the SPLIT INTO clause to specify the number of tablets to be created for the index. The hash range is then evenly split across those tablets.

Presplitting indexes, using SPLIT INTO, distributes index workloads on a production cluster. For example, if you have 3 servers, splitting the index into 30 tablets can provide higher write throughput on the index. For an example, see [Create an index specifying the number of tablets](#create-an-index-specifying-the-number-of-tablets).

{{< note title="Note" >}}

By default, YugabyteDB presplits an index into `ysql_num_shards_per_tserver * num_of_tserver` tablets. The SPLIT INTO clause can be used to override that setting on a per-index basis.

{{< /note >}}

### SPLIT AT VALUES

For range-sharded indexes, you can use the SPLIT AT VALUES clause to set split points to presplit range-sharded indexes.

**Example**

```plpgsql
CREATE TABLE tbl(
  a INT,
  b INT,
  PRIMARY KEY(a ASC, b DESC)
);

CREATE INDEX idx1 ON tbl(b ASC, a DESC) SPLIT AT VALUES((100), (200), (200, 5));
```

In the example above, there are three split points, so four tablets will be created for the index:

- tablet 1: `b=<lowest>, a=<lowest>` to `b=100, a=<lowest>`
- tablet 2: `b=100, a=<lowest>` to `b=200, a=<lowest>`
- tablet 3: `b=200, a=<lowest>` to `b=200, a=5`
- tablet 4: `b=200, a=5` to `b=<highest>, a=<highest>`

{{< note title="Note" >}}

By default, YugabyteDB creates a range sharded index as a single tablet. The SPLIT AT clause can be used to override that setting on a per-index basis.

{{< /note >}}

## Examples

### Unique index with HASH column ordering

Create a unique index with hash ordered columns.

```plpgsql
CREATE TABLE products(id int PRIMARY KEY,
                                 name text,
                                 code text);
CREATE UNIQUE INDEX ON products(code);
\d products
```

```output
              Table "public.products"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 id     | integer |           | not null |
 name   | text    |           |          |
 code   | text    |           |          |
Indexes:
    "products_pkey" PRIMARY KEY, lsm (id HASH)
    "products_code_idx" UNIQUE, lsm (code HASH)
```

### ASC ordered index

Create an index with ascending ordered key.

```plpgsql
CREATE INDEX products_name ON products(name ASC);
\d products_name
```

```output
   Index "public.products_name"
 Column | Type | Key? | Definition
--------+------+------+------------
 name   | text | yes  | name
lsm, for table "public.products
```

### INCLUDE columns

Create an index with ascending ordered key and include other columns as non-key columns

```plpgsql
CREATE INDEX products_name_code ON products(name) INCLUDE (code);
\d products_name_code;
```

```output
 Index "public.products_name_code"
 Column | Type | Key? | Definition
--------+------+------+------------
 name   | text | yes  | name
 code   | text | no   | code
lsm, for table "public.products"
```

### Create an index specifying the number of tablets

To specify the number of tablets for an index, you can use the CREATE INDEX statement with the [SPLIT INTO](#split-into) clause.

```plpgsql
CREATE TABLE employees (id int PRIMARY KEY, first_name TEXT, last_name TEXT) SPLIT INTO 10 TABLETS;
CREATE INDEX ON employees(first_name, last_name) SPLIT INTO 10 TABLETS;
```

### Partial indexes

Consider an application maintaining shipments information. It has a `shipments` table with a column for `delivery_status`. If the application needs to access in-flight shipments frequently, then it can use a partial index to exclude rows whose shipment status is `delivered`.

```plpgsql
CREATE TABLE shipments (id int, delivery_status text, address text, delivery_date date);
CREATE INDEX shipment_delivery ON shipments(delivery_status, address, delivery_date) WHERE delivery_status != 'delivered';
```

### Expression indexes

An index column need not be just a column of the underlying table; it can also be a function, or scalar expression computed from one or more columns of the table. You can also obtain fast access to tables based on the results of computations.

A basic example is indexing unique emails in a users table similar to the following:

```plpgsql
CREATE TABLE users(id BIGSERIAL PRIMARY KEY, email TEXT NOT NULL);

CREATE UNIQUE INDEX users_email_idx ON users(lower(email));
```

Creating a unique index prevents inserting duplicate email addresses using a different case.

Note that index expressions are only evaluated at index time, so to use the index for a specific query the expression must match exactly.

```plpgsql
SELECT * FROM users WHERE lower(email)='user@example.com'; # will use the index created above
SELECT * FROM users WHERE email='user@example.com'; # will NOT use the index
```

Expression indexes are often used to [index jsonb columns](../../../datatypes/type_json/create-indexes-check-constraints/).

## Troubleshooting

If the following troubleshooting tips don't resolve your issue, ask for help in our [community Slack]({{<slack-invite>}}) or [file a GitHub issue](https://github.com/yugabyte/yugabyte-db/issues/new?title=Index+backfill+failure).

### Invalid index

If online CREATE INDEX fails, an invalid index may be left behind. These indexes are not usable in queries and cause internal operations, so they should be dropped.

For example, the following commands can create an invalid index:

```plpgsql
CREATE TABLE uniqueerror (i int);
```

```output
CREATE TABLE
```

```plpgsql
INSERT INTO uniqueerror VALUES (1), (1);
```

```output
INSERT 0 2
```

```plpgsql
CREATE UNIQUE INDEX ON uniqueerror (i);
```

```output
ERROR:  ERROR:  duplicate key value violates unique constraint "uniqueerror_i_idx"
```

```plpgsql
\d uniqueerror
```

```output
            Table "public.uniqueerror"
 Column |  Type   | Collation | Nullable | Default
--------+---------+-----------+----------+---------
 i      | integer |           |          |
Indexes:
    "uniqueerror_i_idx" UNIQUE, lsm (i HASH) INVALID
```

Drop the invalid index as follows:

```plpgsql
DROP INDEX uniqueerror_i_idx;
```

```output
DROP INDEX
```

### Common errors and solutions

- `ERROR:  duplicate key value violates unique constraint "uniqueerror_i_idx"`

  **Reason**: When creating a [unique index](#unique), a unique constraint violation was found.

  **Fix**: Resolve the conflicting row(s).

- `ERROR:  Backfilling indexes { timeoutmaster_i_idx } for tablet 42e3857759f54733a47e3bb817636f60 from key '' in state kFailed`

  **Reason**: Server-side backfill timeout is repeatedly hit.

  **Fixes**

  Do any or all of the following:

  - Increase the YB-Master flag [ysql_index_backfill_rpc_timeout_ms](../../../../../reference/configuration/yb-master/#ysql-index-backfill-rpc-timeout-ms) from 60000 (one minute) to 300000 (five minutes).
  - Tune the YB-TServer flag [backfill_index_timeout_grace_margin_ms](../../../../../reference/configuration/yb-tserver/#backfill-index-timeout-grace-margin-ms): with `-1` (default), YSQL uses an automatic margin of at least 3 minutes (180000 ms); YCQL uses about 1 second. To set an explicit margin, use a value such as `60000` (one minute).
  - Decrease the YB-TServer flag [backfill_index_write_batch_size](../../../../../reference/configuration/yb-tserver/#backfill-index-write-batch-size) from 128 to 32.

- `ERROR:  BackfillIndex RPC (request call id 123) to 127.0.0.1:9100 timed out after 86400.000s`

  **Reason**: Client-side backfill timeout is hit.

  **Fixes**

  The master leader may have changed during backfill. This is currently [not supported][backfill-master-failover-issue]. Retry creating the index, and keep an eye on the master leader.

  Try increasing parallelism. Index backfill happens in parallel across each tablet of the table. A one-tablet table in an [RF-3][rf] setup would not take advantage of the parallelism. (One-tablet tables are default for range-partitioned tables and colocated tables.) On the other hand, no matter how much parallelism there is, a one-tablet index would be a bottleneck for index backfill writes. Partitioning could be improved with [tablet splitting][tablet-splitting].

  In case the backfill really needs more time, increase YB-TServer flag [backfill_index_client_rpc_timeout_ms](../../../../../reference/configuration/yb-tserver/#backfill-index-client-rpc-timeout-ms) to as long as you expect the backfill to take (for example, one week).

**To prioritize keeping other transactions alive** during the index backfill, set each of the following to be longer than the longest transaction anticipated:

- YB-Master flag [index_backfill_wait_for_old_txns_ms](../../../../../reference/configuration/all-flags-yb-master/#index-backfill-wait-for-old-txns-ms)
- YSQL parameter `yb_index_state_flags_update_delay`

**To speed up index creation** by a few seconds when you know there will be no online writes, set the YSQL parameter `yb_index_state_flags_update_delay` to zero.

[backfill-master-failover-issue]: https://github.com/yugabyte/yugabyte-db/issues/6218
[rf]: ../../../../../architecture/docdb-replication/replication/#replication-factor
[tablet-splitting]: ../../../../../architecture/docdb-sharding/tablet-splitting
[yb-master]: ../../../../../reference/configuration/yb-master/
[yb-tserver]: ../../../../../reference/configuration/yb-tserver/

## See also

- [ALTER INDEX](../ddl_alter_index)
- [DROP INDEX](../ddl_drop_index)
- [Faster writes to new tables](../../../../../explore/transactions/new-table-writes/)
