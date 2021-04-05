Tracking issue: [#4192](https://github.com/yugabyte/yugabyte-db/issues/4192)

# Online Schema Migrations

Most applications have a need to constantly evolve the database schema, while simultaneously not being able to take any downtime. Therefore, there is a need for schema migrations (which involve DDL operations) to be safely run concurrently with foreground operations. In case of a failure, the schema change should be rolled back and not leave the database in a partially modified state.

> **NOTE:** A schema migration (or DDL operation) that is performed concurrently with foreground DML operations is henceforth referred to as an *online* schema migration.

# Goals

The goals of this feature are as follows.

* **Safety for online DDL operations**
    * Identify the set of DDL operations (run as a part of schema migrations) that can run safely and in an online manner.
    * Support more DDL operations that can be run safely in an online manner.
    * In the case of DDL statement failures, the DB should revert to the state before the DDL operation started.

* **Consistency of schema for concurrently running transactions**
    * The DB should expose a consistent view of the schema at all points of time, including the period when the change is happening. 
    * While a schema change could be considered a long running process, any concurrently running transaction should execute with a fixed schema for its entire lifetime.

* **Observability**
    * The user should be able to inspect the status of the DDL change including determing when it is completed
    * The user should be able to determine the reason for the failure of the DDL operation.
    * In case of failures, the semantics of rollback should be clear. For example, if an `ADD CONSTRAINT` operation fails, the reason for the failure such as a constraint violation should be stated.

* **General purpose framework**

    Schema migrations can be handled by frameworks or initiated as DDL statements. The framework should work across all of these:

    * DDL statements issued by applications, including initiated by the user from a shell (example: `ysqlsh` or `psql`)
    * Schema migrations performed by ORM frameworks (example: Spring/Hibernate, Django, etc)
    * Standalone database migration software libraries (example: [Flyway](https://flywaydb.org/) and [Liquibase](https://www.liquibase.org/))


> **Note:** There are a number of DDL operations currently supported by YugabyteDB, including many more that are planned in the roadmap. The aim is to increase the coverage for DDL operations that are safe to run in an online manner.


# Motivation

Traditionally, schema migrations were handled by taking all services down during the upgrade. A migration script would run to apply the new schema. Depending on the exact schema change, there could be a step to transform all the data before the schema migration completes. THe services are brought back online after this.

While the approach is very safe, it leads to a lot of downtime in practice, especially in a distributed SQL database like YugabyteDB for the following reasons:

* Simple DDL operations could take longer because the schema change needs to be applied across a cluster of nodes. This is especially so in clusters with a large number of nodes.
* Some DDL operations (such as adding an index or constraint) would need to transform data. Since the data set sizes can be quite large, transforming the data could take a very long time.
* There could be a number of DDL operations that are executed as a part of the schema migration.

The design for online schema migration is based on [the Google F1 design for online, asynchronous schema changes](https://storage.googleapis.com/pub-tools-public-publication-data/pdf/41376.pdf).

# Safe and unsafe DDL Operations

Below is an outline of the various DDL operations that are supported by YSQL and YCQL today (the expectation is that YSQL will end up supporting all the operations eventually even though this is not the case today). The aim is to make all these operations safe for online schema changes. 

| Operation Type | DDL Operation            | YSQL Support | YCQL Support  | Is operation safe (YSQL) | Is operation safe (YCQL) |
| :--------: |-------------------------------------------------- |:--:|:--:|:------------------------:|:------------------------:|
|`COLUMN`    | Add a column                                      | ✅ | ✅ | v2.3 | ✅ |
|`COLUMN`    | Drop an existing column                           | ✅ | ✅ | v2.3 | ✅ |
|`COLUMN`    | Add a column with a default value                 | ✅ | :x:| :x:|  |
|`COLUMN`    | Change default value of a column                  | ✅ | :x:| ✅ |  |
|`COLUMN`    | Add a column that is non-nullable                 | ✅ | :x:| v2.3|  |
|`COLUMN`    | Change the type of a column                       | :x:| :x:|  |  |
|`CONSTRAINT`| Add a unique constraint                           | ✅ | ✅ | v2.3| ✅ |
|`CONSTRAINT`| Add non-unique constraints (CHECK, NOT NULL, etc) | ✅ | :x:| v2.3|  |
|`CONSTRAINT`| Drop constraint                                   | ✅ | ✅ | v2.3|  |
|`INDEX`     | Add index (not primary key)                       | ✅ | ✅ | v2.2 | ✅ |
|`INDEX`     | Add unique index                                  | ✅ | ✅ | v2.3 | ✅ |
|`INDEX`     | Add primary key                                   | :x: | :x: |  |  |
|`INDEX`     | Alter primary key                                 | :x: | :x: |  |  |
|`INDEX`     | Drop index                                        | ✅ | ✅ | v2.3 | ✅ |
|`TABLE`     | Create table                                      | ✅ | ✅ | v2.3 | ✅  |
|`TABLE`     | Rename table                                      | ✅ | :x:| v2.3 |   |
|`TABLE`     | Drop table                                        | ✅ | ✅ | v2.3 | ✅ |

This section first introduces a the generalized framework for performing online schema changes. The subsequent sections will discuss how this framework is used to carry out the specific operations.

# Design - General purpose schema change framework

## Overview

Each YugabyteDB node has an instance of the YSQL query layer which receives, parses and analyzes queries to produce an execution plan, and the DocDB storage layer which handles storing, replicating and retrieving data.
Additionally, the cluster metadata (e.g. the schemas of the relations) is stored DocDB in such a way that at any point one node acts as the metadata-leader (or master leader).

> **Note:** For a detailed description of Yugabyte YSQL architecture see the following posts focusing on the storage layer and the YSQL query layer.

**Definition** A database representation d is consistent with respect to a schema S if:
  1. all key-values pairs belong to either a table or index in S 
  2. all column key-value pairs correspond to a column in its table or index
  3. all index key-value pairs point to existing rows in the primary table
  4. all rows have values for all required columns (e.g. primary key, index key).
  5. all public indexes are complete (have entries for all rows in the primary table, except for excluded rows for partial indexes)
  6. all public constraints are honoured

Below, we will say that a cluster is consistent (or in a consistent state) to imply that its database representation d is consistent with respect to its schema S (i.e. the schema as present on its master leader).

Based on the definition above there are two main anomalies:
* orphan data anomaly database representation contains a key-value pair that it should not contain according to schema S (corresponding to items 1,2,3, above).
* data integrity anomaly database representation is missing a key-value pair that it should contain or it contains a key-value pair that violates a constraint in schema S (corresponding to items 4,5,6 above).

> **Note:** We use the same general approach and definitions as the F1 paper. Therefore, we give a simplified accounting of the common fundamentals (see the paper for more details on that), and instead focus on the Yugabyte/YSQL-specific aspects here.

Every DDL statement is executed internally as a schema migration, meaning a sequence of schema change operations S1 → S2 → S3 → … → Sn (described below).

In next section below we describe safe, online schema migrations and how they are implemented in YSQL. In the subsequent section, we show how DDL statements can be modeled as schema migrations so that they can be executed in an online manner, while preserving the consistency of the cluster. Finally, we summarize the properties and guarantees of schema migrations in YSQL.

## Applying schema migrations

In order to guarantee safe, online schema changes we need to provide an algorithm for executing schema migrations that guarantees that, for each node, the database representation remains consistent with its current schema throughout the schema-migration.

First, we define the notion of preserving consistency for a single schema change step.

#### Definition 1
A schema change from schema S1 → S2 is consistency preserving iff, for any database representation d consistent with respect to both S1 and S2, it is true that 
* any operation op(S1) preserves the consistency of d with respect to schema S2, and 
* any operation op(S2) preserves the consistency of d with respect to schema S1.
where we use op(Si) to denote an operation parsed, analyzed and executed with respect to schema Si.

*Corollary 1:* If S1 → S2 is consistency-preserving then S1 → S2 is also consistency preserving. This follows trivially from definition 1 as it is symmetric with respect to S1 and S2. 

Now, we can define the notion of preserving consistency for a schema migration.

#### Definition 2
A schema migration S = S1 → S2 → S3 → … → Sn is consistency-preserving if and only if 
At any point there is at most one schema change operation Si → Si+1 ongoing on a cluster.
All schema change operations Si → Si+1 are consistency-preserving.

> **Note:** The "one schema change" property above is stronger than strictly required. The minimal-required invariant is that any valid operation operation will be parser/analyzed with a schema version that is at most one version away from the version of the data it affects (read/write). This obviously follows from the  "one schema change" property but is a weaker requirement.

For instance, if two schema migrations are provably independent, in the sense that no operations cannot touch objects that are affected by both migrations then we could safely execute both migrations at the same time. A trivial example of provably-independent schema migrations are two DDL statements that modify relations in different databases (due to database-level isolation guaranteed by YSQL).

But, for simplicity, we assume one change at a time below.

### Applying a schema change

We store schema information in two places: the YSQL system catalogs, and the DocDB schema protobufs. The respective local caches on the nodes for that schema information are also handled differently: for YSQL the Postgres-layer system-catalog caches and for DocDB the PgGate-layer PgSession caches).
Therefore, we distinguish two kinds of schema changes below: YSQL and DocDB. Note that schema migrations will typically involve both YSQL and DocDB schema changes.

In both cases, we consider a change fully applied when the relevant metadata is updated and all the local caches are refreshed accordingly (since the caches determine the effective schema version for the query processing on each node).
Therefore, in order to preserve consistency as per Definition 2 we must ensure not only that the change is applied, but also that the relevant caches are updated before proceeding with the next change. 


#### DocDB schema change
A DocDB schema change consists of changes to the schema protobuf on the master leader and on each tablet on each node. 

1. Execute schema update on master leader

    a. Write the new schema to syscatalog (CREATING/ALTERING/DELETING state) .
    b. Notify all nodes (i.e. DocDB tablets) to apply change Si → Si+1
    c. Each Ti will apply the change and increment its version to Si+1
    d. Wait for all tablets to confirm change has been successfully applied.

2. Update caches

    a. Each node's  (query layer) cache will be updated via an error/retry mechanism when the DocDB version does not match with the cached version  (see implementation limitations below).


#### Implementation limitations

The schema version match for cache invalidation is too strict, it checks for equality (i.e. same schema version) rather than compatibility (i.e. schema versions that are consecutive as per Definition 1). This adds some unavailability (which is handled internally via query retries) but does not affect the correctness of the algorithm.

### YSQL schema change

A YSQL schema change consists of a transaction including one or more writes to YSQL system catalog tables (such as `pg_class`, `pg_attribute`, `pg_constraint`, etc.) which are stored on the master leader.

The change then gets applied on the individual nodes, by simply updating the local caches on each (connection process for each) node. This is done by maintaining the schema version (`ysql_catalog_version`) for every node and ensuring it gets updated when the master leader version gets updated.

Specifically, the steps are:

1. Execute update on master leader

    a. Begin transaction
    b. Execute writes for Si → Si+1  to system catalog tables
    c. Commit transaction
    d. Increment ysql_catalog_version

2. Update caches

    a. Notify each node that schema version was incremented (piggybacking on nodes (master-tserver) heartbeat responses).
    b. Like for DocDB, each node's  (query layer) YSQL cache will be updated via an error/retry mechanism when the receiving node's version does not match with the cached version (see implementation limitations below).

### Implementation limitations
First, as for DocDB changes, the schema version match for cache invalidation is too strict, it checks for equality rather than compatibility.
Second, the master leader does not wait for the nodes (query layers) to be notified of the change. So it could proceed with another change during a migration before guaranteeing that the previous one has been finished. To guarantee correctness as per the definitions above, this needs to be fixed. Current idea is to wait for heartbeats confirming the schema version of each node (tserver) before proceeding (for a max of some ysql_catalog_version lease time).


**Theorem 1:** Applying one or more consistency-preserving schema migrations preserves the consistency of a  cluster. 

Proof sketch: Other than the mentioned limitations, that process described above will ensure that at any point,
there is at most one schema-change happening in the cluster. Specifically, because the master leader will wait for confirmation from the cache layers before proceeding to the next schema change step during any migration. 
Even more, it ensures that at any point, the query layer information (i.e. the caches for the DocDB and YSQL schemas) will be either the exact version as the master version, or one version behind. That means any operation op(Si) will be applied to data that is either at version Si or Si+1. 
Therefore, it directly follows from the definitional property of consistency-preserving changes, the consistency of the cluster will be preserved.

## Executing DDL statements

Conceptually, each schema migration will involve one or more of the following steps:

* Schema-only change - metadata modification on the master leader
* Data reorg - data modification in DocDB
* Data validation - data check in DocDB

Based on that we distinguish the following types of schema migration based on the change steps they require: schema-only migration, schema migration with data validation, and schema migration with data reorganization.
Note that the latter case, schema migration with data reorganization, is the most complex case in terms of the changes steps required and therefore can be considered to subsume the other cases. However, for the clarity of the overall design and the simplicity (and efficiency) of the implementation, it is valuable to consider them separately. 

For each schema migration type, we can either add a structural element or remove a structural element. A structural element  can be either a table, an index, a function/operator, a column, a column property, or a constraint.

Due to Corollary 1 above ( If S1 → S2 is consistency-preserving then S2 → S1 is also consistency preserving) we can construct the schema migration for the remove case as the reverse of the schema migration of the add case.

Moreover, based on the same property, we can also define the "undo" schema migrations for aborting (partially applied) schema migrations.

**Definition 3** (Aborting schema migrations). For any schema migration S = S1 → S2 → S3 → … → Sn, if its execution fails at step Sk → Sk+1 (i.e. not applying Sk+1) we can safely abort/undo the schema migration by applying the schema migration SU = Sk → Sk-1 → … → S1.

*Proof:* The fact that SU will undo the migration S1 → S2 → S3 → … → Sk  is straightforward from its definition. The fact that SU is consistency-preserving, follows from Corollary 1.





### Schema-only migration
A schema-only migration is a schema migration involving only schema-only changes.
For example, creating or dropping a table, function, or operator will not require either reorganization or validation of any data. The data stores (i.e. tablets) might need to be created or deleted (i.e. for tables) but there is no online data reorganization or validation needed.

#### Execution steps

Add structural element: `ABSENT` → `CREATED` → `PUBLIC`
Remove structural element: `PUBLIC` → `CREATED` → `ABSENT`

**Theorem 2:** Every schema change step Si → Si+1 for a schema-only migration is consistency-preserving.

*Proof* We look at each schema change step for the add case individually. The remove case follows trivially from the add case based on Corollary 1.

`ABSENT` → `CREATED`
Both in the absent and created states the query layer will not send any read or write operations involving that structural element. So no orphan data or data integrity anomaly can be created because there will be no data.

`CREATED` → `PUBLIC`
Both in the created and public states the structural element exists so there can be no orphan data anomaly. With respect to data integrity constraints we have two scenarios:

* for integrity constraints that should be checked by data layer (e.g. unique constraints) the CREATED/PUBLIC states will have and check those constraints.
* for query-layer integrity constraints (e.g. check constraints), if the query layer is in PUBLIC state it will check them as expected. If it is not in PUBLIC state (i.e. CREATED) it will not allow any reads or writes anyway so it cannot cause an integrity anomaly.

#### Examples

Since YSQL has both DocDB and YSQL metadata for the same relation. We define the states relative to that. Specifically:
* `ABSENT` schema missing in YSQL and in DocDB
* `CREATED` schema in DocDB but not in YSQL
* `PUBLIC` schema in both DocDB and YSQL

**`CREATE TABLE`**
Create table schema in DocDB (`ABSENT` → `CREATED`)
Create table schema in YSQL  (`CREATED` → `PUBLIC`)

**`DROP TABLE`**
Delete table schema from YSQL  (`PUBLIC` → `CREATED`)
Delete table schema from DocDB (`CREATED` → `ABSENT`)

#### Schema migration with data validation
A scheme migration with data validation is a migration that involves at least one data validation step (i.e. reading and checking data) but no data reorganization step (i.e. modifying/writing data).
For example, adding a check constraint to a table, or adding a foreign key constraint are schema migrations with data validation.

**Execution steps**
* Add structural element: `ABSENT` → `CHECK` → `DB_VALIDATE` → `PUBLIC`
* Remove structural element: `PUBLIC` → `CHECK` → `ABSENT`

> **Note:** Above we consider DB_VALIDATE as a background operation happening as part of the CHECK → PUBLIC transition, rather than an explicit step, but denote it as a standalone step for notational simplicity.

**Theorem 3:** Every schema change step Si → Si+1 for a schema migration with data validation is consistency-preserving.

*Proof:* We look at each schema change step for the add case individually. The remove case follows trivially from the add case based on Corollary 1.

`ABSENT` → `CHECK`

1. If the query layer is in ABSENT state then no data will get checked (because the query layer will not acknowledge that constraint exists). This is compatible with data being in check state, because the check state does not require the data to be valid w.r.t. to the constraint.

2. If the query layer is in CHECK state then it will check all new data, and reject some queries. Therefore it cannot cause any anomalies.

`CHECK` → `DB_VALIDATE` → `PUBLIC`
Both `CHECK` and `PUBLIC` acknowledge the existence of the constraint so, from the read-point hybrid-time ht onwards, `WRITE` state operations cannot introduce any new integrity anomalies (as ops run in both states will check new writes). Additionally, the `DB_VALIDATE` process will guarantee it will resolve/check all integrity anomalies before ht (or fail in which case we abort/undo the change). Considering the requirement that the promotion to `PUBLIC` cannot happen until the `DB_VALIDATE` has completed we can guarantee that this schema change introduces no anomalies.

#### Implementation

The metadata for foreign key and check constraints is only stored as YSQL metadata. DocDB metadata only needs storage-related metadata (such as number of columns, column types, primary key, etc). DocDB does know about unique (and primary key) constraints though, but these are handled differently, i.e. as adding/removing indexes).

Therefore, in the YSQL implementation, the states above map as follows:

* `ABSENT` check constraint not set.
* `CHECK` check constraint set in YSQL but in "not checked" state.
* `DB_VALIDATE` background validation process running.
* `PUBLIC` check constraint set in YSQL in "checked" state.

#### Examples

**`ADD CHECK CONSTRAINT`**
Add YSQL check constraint with the convalidated column in pg_constraint set to false.
Run background validation process.
Update the constraint's pg_constraint entry to set convalidated to true.

**`DELETE CHECK CONSTRAINT`**
Update the constraint's pg_constraint to set convalidated to false.
Remove the constraint from YSQL.

Note that foreign key constraints are handled similarly to CHECK constraints with two differences:

* In the CHECK state the we need to check writes coming into two tables (the base table and the referenced table). This should be automatically handled by YSQL layer once the pg_constraint entry is added.
* Similarly, the background validation needs to check (i.e. read from) both tables to ensure they are consistent with each-other.

#### Schema migration with data reorganization

A schema migration with data reorganization is any schema migration that involves at least one data reorganization change step  (i.e. modifying/writing data).
For example, creating a secondary index, or adding a column with (write-) default value are schema migrations with data reorganization.

Add structural element: `ABSENT` → `DELETE` → `WRITE` → `DB_REORG` → `PUBLIC`
Remove structural element: `PUBLIC` → `WRITE` → `DELETE` → `ABSENT`

*Note:* Above we consider DB_REORG as a background operation happening as part of the `WRITE` → `PUBLIC` transition, rather than an explicit step, but denote it as a standalone step for simplicity.

**Theorem 2** Every schema change step Si → Si+1 for a schema migration with data reorganization is consistency-preserving.

*Proof:* We look at each schema change step for the add case individually. The remove case follows trivially from the add case based on Corollary 1.

**`ABSENT` → `DELETE`**

1. If the query layer is in ABSENT state then no data will get written (because query layer will not acknowledge that element exists) which is compatible with the data being in DELETE state.
2. If the query layer is in DELETE state then it will only allow deletes and therefore will not add any DocDB key-value pairs, only deletes. Therefore there can be no orphaned-data anomaly w.r.t the ABSENT state.

DELETE → WRITE
Since both DELETE and WRITE acknowledge the existence of the structural element they cannot cause orphaned-data anomaly.
W.r.t data-integrity anomaly, both states will ensure DELETEs go through which will avoid the 
false-positive unique-constraint violation anomaly -- that going directly from ABSENT → WRITE could have caused for the case: write_k(DELETE), delete_k(ABSENT), write_k(WRITE). 

**`WRITE → DB_REORG → PUBLIC`**
Both WRITE and PUBLIC acknowledge the existence of the structural element they cannot cause orphaned-data anomaly.
Furthermore, from the read-point hybrid-time ht onwards, WRITE state operations cannot introduce any new integrity anomalies (as they will do all writes to the index and if needed check unique constraints). Additionally, the DB_REORG process will guarantee it will resolve/check all integrity anomalies before ht (or fail in which case we abort/undo the change). 
Considering the requirement that the promotion to PUBLIC cannot happen until the backfill has completed this guarantees that this introduces no anomalies.

#### Examples

**`CREATE INDEX`**

1. `ABSENT` → `DELETE` is done in two steps for now due to YSQL vs DocDB metadata

    a. Create the index in DocDB in `DELETE` state.
    b. Create the index in YSQL with indisready=true (ready for writes) and indisvalid=false (not ready for reads).

2. Set the DocDB index metadata into `DELETE+WRITE` state.

3. Run index backfill process.

4. `WRITE/DB_REORG` → `PUBLIC` done in two steps for now due to YSQL vs DocDB metadata

    a. Set the DocDB index metadata to READY state.
    b. Update the YSQL index metadata to set indisvalid=true (ready for reads).


**`DROP INDEX`**

1. `PUBLIC` → `WRITE `

    a. Update the YSQL index metadata to set indisvalid=false (disable reads).
    b. Set the DocDB index metadata to DELETE+WRITE state.

2. Set the DocDB index metadata into DELETE state.

3. DELETE → ABSENT
    a. delete the index from YSQL metadata
    b. delete the index from DocDB.




# References

* [Online, Asynchonous Schema Change in F1](https://storage.googleapis.com/pub-tools-public-publication-data/pdf/41376.pdf)
* [Safe and unsafe operations for high volume PostgreSQL](https://leopard.in.ua/2016/09/20/safe-and-unsafe-operations-postgresql)


[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/design/online-schema-migrations.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
