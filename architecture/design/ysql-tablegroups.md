# Tablegroups

> **Note:** This is a new feature in Beta mode.

"In workloads that do very little IOPS and have a small data set, the bottleneck shifts from CPU/disk/network to the number of tablets one can host per node. Since each table by default requires at least one tablet per node, a YugabyteDB cluster with 5000 relations (tables, indexes) will result in 5000 tablets per node. There are practical limitations to the number of tablets that YugabyteDB can handle per node since each tablet adds some CPU, disk and network overhead. If most or all of the tables in YugabyteDB cluster are small tables, then having separate tablets for each table unnecessarily adds pressure on CPU, network and disk.

To help accomodate such relational tables and workloads, we've added support for colocating SQL tables. Colocating tables puts all of their data into a single tablet, called the  _colocation tablet_. This can dramatically increase the number of relations (tables, indexes, etc) that can be supported per node while keeping the number of tablets per node low. Note that all the data in the colocation tablet is still replicated across 3 nodes (or whatever the replication factor is)." - From the existing design of [colocated tables](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/ysql-colocated-tables.md).

We have extended this concept to something aptly titled "tablegroups." A tablegroup is a group of tables that will be colocated together on a single *colocation tablet*. This can be specified at the language level in YSQL. This will allow users to create multiple colocation tablets per database.
## Motivation
The motivation behind having multiple groups of co-located tables within a database is similar to that of specifying a database as co-located. Currently, in specifying a database as co-located, we are able to generate a single tablet that all future tables (that do not opt out of co-location) are added to. This allows a user to co-locate closely related tables together and avoid paying the cost of additional round trips when making joins etc. amongst the related data or paying the storage/compute overhead of creating a tablet for every relation, improving performance.

However, this current implementation has a few pitfalls. There is sometimes a need to horizontally scale groups of tables together, especially as the colocation tablet containing them grows excessively large. There are also needs to co-locate multiple different groups of tables (i.e. per-schema) without requiring them all to be on one tablet. This can help improve scalability while still benefitting from the latency improvements afforded by colocation.

This problem can be addressed three ways - all under the concept of forming tablegroups. We have implemented the first *(currently in beta)* and are beginning a conversation around which of the latter two we will eventually support.

 1. **Tablegroup Colocation**
    - **Summary:** Colocating tables together within a tablegroup allows for related tables to be placed on one tablet (without restrictions on their primary keys).
    - **Pros:**
         - Along with the benefits of placing a group of tables together on one colocated tablet - i.e. that # relations per node can increase while keeping #tablets low - adding the explicit concept of a tablegroup allows for a user to take advantage of colocation without completely losing the ability to scale out. If there are multiple independent groups of tables that need to be colocated, the status quo will limit scalability.
    - **Cons:**
        - The problem is that all tables in the tablegroup are placed on a single tablet (one RocksDB) and are replicated throughout the cluster but cannot be partitioned.
        - In order to scale this tablet horizontally (i.e. if the number of rows start to grow), one of the following options (co-partitioning / interleaving) will need to be specified. However, for co-partitioning or interleaving to be possible, there must be a relationship between the partition keys of the tables being co-partitioned or interleaved.
2.  **Co-partitioning data**
    - **Summary:** Co-partitioning tables will partition tables in a tablegroup together. However, in order to do this tables must share a "common" partition key (types & number of columns). Rows from different tables with the same hash of the partition key (or whose partition key falls within the same range, depending on how tablet splitting is defined for the tablegroup) will be placed on the same tablet. The primary conceptual difference between co-partitioning and interleaving (the option below) is that the layout of how the data is stored in RocksDB will differ.
    - **Pros:**
        - Compared to simple co-located tablegroups, co-partitioning data will allow for increased scalability. Full table scans will be more performant than with interleaving.
    - **Cons:**
        - The improvements in scalability will come at the cost of some latency due to roundtrips to other tservers in executing queries when tablets are split and distributed.
3. **Interleaving data**
    - **Summary:** Interleaving will enforce a strict hierarchical relationship between tables in the tablegroup, and child tables will have to have parent table partition keys as a prefix of their own. When data is written to a child table, it is inserted into the KV store following the entry from the parent table with the same shared prefix in the partition key. In this sense data will be interleaved when written on disk. Thus this method of storing data is natural when a user’s tables form a hierarchy.
    - **Pros:**
        - Same scalability improvements as co-partitioning. Joins will be more performant than with co-partitioning.
    - **Cons:**
        - Same latency costs as co-partitioning (compared to basic colocation).

# Implementation Details

## Catalog Manager Changes

When a tablegroup is created, the catalog manager should create a parent table and parent tablet for this tablegroup for colocation. This will reuse code from the initial commit of database-level colocation to support multiple tables for a single tablet. Same idea of a dummy table as the system tables using the sys catalog table (and kSysCatalogTableId as primary_table_id). These changes should be similar to the CREATE DATABASE changes found [here](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/ysql-colocated-tables.md).

Catalog manager will have a per-namespace map of `tablegroup_id` to the `TabletInfo` for that tablegroup. This `tablegroup_id` is a 16-byte Yugabyte UUID that is derived from the `tablegroup oid` and `database oid` in the same way that `PgsqlTableId` is derived.
This will initially look like `map<tablegroup_id, tablet_id> tablegroup_tablets`; and `map<namespace_uuid, tablegroup_tablets> tablegroup_ids_map`; Eventually `tablegroup_tablets` will be changed to be `map<tablegroup_uuid, vector<tablet_id>>` if interleaving or co-partitioning is implemented.

Catalog manager will also maintain some in-memory metadata for tablegroups.

In the event of a restart, `catalog_loaders` will be able to reload this information based on the persistent table and tablet info.

## Postgres system metadata
There are two things that need to be done here:
1. Add a new system catalog to store metadata about the tablegroups within a database.
2. Either add a column to `pg_class` or re-use existing columns in `pg_class` to store the per-relation tablegroup information.

For the new system catalog we will do the following:
Add a `pg_tablegroup` system catalog that contains the `oid, grpname grpacl, grpowner, and grpoptions` of each tablegroup. In order to ensure backwards compatibility we will be checking whether the physical table exists in `postinit` and if so, set a Postgres global. Elsewhere in the codebase anytime where a tablegroup-related operation needs to be performed, this PG global will be checked to guard opening the relation.

For the per-relation information there are a couple options:
There is the option to use the `reltablespace` field of `pg_class`. In this column, we would be able to store the grpoid corresponding to the tablegroup that the table is to be a part of. This likely is not the best long-term solution especially since we intend to implement tablespaces to handle placement configurations / GDPR related requirements.

The related option (and likely simplest solution to implement) would be to add another column that would represent the tablegroup that a relation is a part of. This could introduce some issues such as needing a default tablegroup / having to create this column for all prod databases. Until we have an `initdb` upgrade path in place, we cannot introduce a `reltablegroup`

The option that we are now choosing is to use `reloptions` to store the per-relation tablegroup ID information. We have added a new `reloption` of type `OID` to store this information.

### CREATE TABLEGROUP

To allow for extensibility for co-partitioning / interleaving, the idea we had in mind is as follows.

1.  CREATE TABLEGROUP name [group_options]
    group_options ::=
    [ COPARTITIONED [ = { ‘true’ | ‘false’ }] (partition_schema) |
    INTERLEAVED [ = { ‘true’ | ‘false’}] ] - both default false
    Where only one of [ COPARTITIONED | INTERLEAVED ] can be true.
2. Currently, neither co-partitioning nor interleaving is supported / group_options are not implemented.

In order to create a tablegroup, a user must either be a superuser or have create privileges on the database the tablegroup is to be created in. At time of tablegroup creation, an OID is generated and existence is checked via a lookup to `pg_tablegroup`. If the tablegroup does not exist, a new tuple is inserted.

We have created a flow for CREATE TABLEGROUP as well as corresponding new RPCs. On the catalog manager side, we will be creating a parent table for the tablegroup with a single tablet. Catalog manager will add entries to the appropriate maps. The prefix for the table name and ID will be the tablegroup ID and the suffix will be a const tablegroup parent ID/name suffix. The colocated property will be set for the table and tablet in order to take advantage of some code reuse.


### DROP TABLEGROUP

A tablegroup must explicitly be dropped and must be empty (no relations associated with it) in order to be dropped. To do so we scan `pg_class` and parse reloptions when non-empty.
 1. DROP TABLEGROUP group_name

On the catalog manager side, corresponding entries are removed from the maps and the parent table & tablet are deleted.

### ALTER TABLEGROUP NAME/OWNER

This will be supported. Will amount to changing the Postgres metadata after checking permissions on the tablegroup.

### GRANT / REVOKE / ALTER DEFAULT PRIVILEGES

RBAC for the tablegroups themselves - `ACL_CREATE` privileges will be grantable for tablegroups.

### CREATE TABLE
If the table is created as CREATE TABLE [...] TABLEGROUP tablegroup_name; then the table will be colocated with the rest of the tables in that tablegroup.

To help avoid confusion between colocated db and tablegroups, both will not be allowed within the same namespace. COLOCATED=true/false as part of the CREATE TABLE ddl will throw an error. If the database was created with COLOCATED=true, then any CREATE TABLE ddl with TABLEGROUP specified will throw an error. Over time we will deprecate COLOCATED=true/false from both CREATE TABLE and CREATE DATABASE and transition to only using TABLEGROUP.

We must also check to make sure that a SPLIT clause is not included as we will not initially support co-partitioning/interleaving. With co-partitioning / interleaving we will need to have additional logic under the hood to ensure that the partition keys are properly specified. In the future we will need to validate that for co-partitioning / interleaving, the partition keys match (in data type & # columns).

Logical flow for CREATE TABLE will look like this:

1. First check for tablegroup existence and permissions. A user must either be an owner of the tablegroup, a superuser, or have create permissions on the tablegroup in order to create a table within one.
2. From the CreateTableRequestPB, catalog manager is passed the tablegroup ID.
3. Perform a lookup for the tablet that this table is to be added to using catalog manager maps.
4. Looks like (for now - until co-partitioning is implemented) will have to convert hash columns to range columns since the tablet will not be able to dynamically split if it is colocated.
5. Set the colocated metadata for the table.
6. Add the table to the tablet (using the AddTableToTablet async RPC added with colocated database commit, ChangeMetadataRequest to replicate the table addition).

### CREATE INDEX
There will be a couple options available for indexes:
1. By default, an index will follow the tablegroup of its indexed table.
2. The index will be able to  opt-out and create its own tablets by providing `NO TABLEGROUP`
3. The index will be able to select a new tablegroup through the `TABLEGROUP group_name` syntax.

### DROP TABLE

Should be similar to [this](https://github.com/yugabyte/yugabyte-db/commit/306f53b38b4683f94d340bd6070793890a92df11) (and [this](https://github.com/yugabyte/yugabyte-db/commit/5a4826ab0531003ac49496c3bdec331490eb6c11)) commit. Will need to remove the map entries from catalog manager and the corresponding info from the Postgres system metadata. Physical removal of table data will happen during compactions.  By setting the colocated property in table and tablet metadata, we should be able to take advantage of earlier implementation here.

In the case of interleaving, there should be a specification whether to drop all children in a cascade or just drop the table and interleave its children with its parent.

### TRUNCATE TABLE

Taking from [here](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/ysql-colocated-tables.md) will be a table-level tombstone. Refer to above commits related to drop table flow for colocation. Need to make sure to use this alternative code path. Reads are already aware of this table-level tombstone.

### ALTER TABLE (add/remove/modify the tablegroup of a table)

This is currently in the works for the status quo implementation of colocation. We will not support this in v1 for tablegroups.

Here are a couple ideas on how to support this:
1.  Extend ALTER TABLE grammar to support new alter_table_action which include removing association with a tablegroup as well as adding association with a tablegroup post creation.
2.  Support an ALTER TABLEGROUP ddl to do the above.

### UI Changes
Modified the master tables UI to display a table for the parent table information and YSQL OID's of the tablegroups (only when tablegroups are present). Similarly, when tablegroups are present, a new column will be in the UI for User tables and Index tables that will display the parent OID if any.
## Future Work
-   Supporting the ability to alter the tablegroup of a table/index.

-   ysql_dump / backup & restore with tablegroups present.

-   Better load balancing tablegroups / colocated tables.

-   Supporting co-partitioning and/or interleaving.

-   Automatic tablet splitting of tablegroup parent tablets.

-   Ability to create a CDC stream per-tablegroup.

-   Initdb upgrade path (not specific to tablegroups, but relevant).

