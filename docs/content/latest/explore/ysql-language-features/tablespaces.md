---
title: Tablespaces
linkTitle: Tablespaces
description: Tablespaces in YSQL
headcontent: Tablespaces in YSQL
image: /images/section_icons/secure/create-roles.png
menu:
  latest:
    identifier: explore-ysql-language-features-tablespaces
    parent: explore-ysql-language-features
    weight: 320
isTocNested: true
showAsideToc: true
---

This document provides an overview of tablespaces in YSQL and demonstrates how they can be used to specify data placement in the cloud. 

This document guides you through the process of creating the following:

- Multi-zone and single-zone tablespaces.
- Tables that use multi-zone and single-zone tablespaces.
- Indexes for tables that use multi-zone and single-zone tablespaces.

## Overview

YSQL tablespaces are entities that specify how data associated with them should be replicated and distributed across cloud, regions, and zones.

In PostgreSQL, tablespaces are used for specifying a location on disk with options to control data access. That is, tablespaces group tables and indexes based on how you intend to store and access the data. For instance, you can choose to place heavily accessed smaller tables and indexes in SSD or “fast” storage compared to other tables.

For YugabyteDB clusters, however, location does not pertain to disk locations. For a cloud-native distributed database, location pertains to the cloud, region, and zone where the data is supposed to be. Therefore, although YSQL tablespaces are built on PostgreSQL tablespaces that allow you to specify placement of data at a table level, this placement information in YSQL defines the number of replicas for a table and index, as well as how they can be distributed across a set of cloud, regions, and zones. 

Note that you cannot use a tablespace outside of the cluster in which it is defined. For information on how to create and configure clusters, see the relevant sections of the following documents:

- [Single-Zone Deployment](https://docs.yugabyte.com/latest/deploy/kubernetes/single-zone/) 
- [Multi-Zone Deployment](https://docs.yugabyte.com/latest/deploy/kubernetes/multi-zone/)

## Defining Tablespaces

You can define a tablespace using the following syntax:

```sql
CREATE TABLESPACE tablespace_name 
  [OWNER username]
  WITH (replica_placement = placement_policy_json);
```

In the preceding syntax:

- *tablespace_name* represents the name of the tablespace to be created. 
- *username* represents the name of the user who will own the tablespace, with the name of the user executing the command being the default. Note that only superusers can create tablespaces and grant their ownership to other types of users. 
- *placement_policy_json* represents a JSON string that specifies the placement policy for this tablespace. The JSON structure contains the following two fields: 
  - `num_replicas` defines the overall replication factor.
  - `placement_blocks` is an array of tuples, with each tuple containing the keys `<”cloud”, “region”, “zone”, “min_num_replicas”>` whose values define a placement block. Typically, the sum of `min_num_replicas` across all placement blocks is expected to be equal to `num_replicas`. The aggregate of `min_num_replicas` can be lesser than `num_replicas`, in which case the extra replicas are placed at the YB-Load balancer’s discretion.

## Creating Tables and Indexes in Tablespaces

You can associate new tables and indexes with a corresponding tablespace. This defines the replication factor of the table or index. It also defines how the replicas of the table or index are to be spread across cloud, regions, and zones.

A table and an index can be created in separate tablespaces.

There is no difference between creating tables and indexes that use multi-zone tablespaces and those that use single-zone tablespaces.

## Single-Zone Tablespaces

The following example shows how to create a single-zone tablespace:

```sql
CREATE TABLESPACE us_west_1a_tablespace 
WITH (replica_placement='{"num_replicas": 3, "placement_blocks":
[{"cloud":"aws","region":"us-west","zone":"us-west-1a","min_num_replicas":3}]}');
```

If you create a table that uses `us_west_1a_tablespace`, each tablet of the table will have three replicas, but unlike [multi-zone tablespaces](multi-zone-tablespaces), all three replicas for any tables and indexes in this tablespaces will be placed within the `us-west-1a` zone.

Using the `us_west_1a_tablespace`, you can apply the YSQL's `TABLESPACE` option to `CREATE TABLE` and `CREATE INDEX` statements, as follows:

```sql
CREATE TABLE employees (
  employee_no integer PRIMARY KEY,
  name text,
  department text,
  change_date date
)
TABLESPACE us_west_1a_tablespace;
```

```sql
CREATE INDEX employee_no_idx ON employees(employee_no) 
TABLESPACE us_west_1a_tablespace;
```

The preceding statements ensure that data in the `employees` and `employee_no_idx` tables is present based on the placement policies specified for `us_west_1a_tablespace`. By default, indexes are placed according to the cluster configuration.

## Multi-Zone Tablespaces

The following example shows how to create a multi-zone tablespace:

```sql
CREATE TABLESPACE us_west_tablespace 
WITH (replica_placement='{"num_replicas": 3, "placement_blocks":
[{"cloud":"aws","region":"us-west","zone":"us-west-1a","min_num_replicas":1},
{"cloud":"aws","region":"us-west","zone":"us-west-1b","min_num_replicas":1},
{"cloud":"aws","region":"us-west","zone":"us-west-1c","min_num_replicas":1}]}');
```

If you create a table and use the preceding tablespace, each tablet of the table will have three replicas, as per the `num_replicas` value. Since `placement_blocks` contains three placement blocks where `min_num_replicas` is set to 1, YB-Load balancer will ensure that `aws.us-west.us-west-1a`, `aws.us-west.us-west-1b`, and `aws.us-west.us-west-1c` will each have one replica.

The following example demonstrates how to create a tablespace in which the value of `min_num_replicas` does not correspond to `num_replicas`:

```sql
CREATE TABLESPACE us_east_tablespace 
WITH (replica_placement='{"num_replicas": 5, "placement_blocks":
[{"cloud":"aws","region":"us-east","zone":"us-east-1a","min_num_replicas":1},
{"cloud":"aws","region":"us-east","zone":"us-east-1b","min_num_replicas":1},
{"cloud":"aws","region":"us-east","zone":"us-east-1c","min_num_replicas":1}]}');
```

In the preceding example, the three `min_num_replicas` fields sum up to 3, whereas the total required replication factor is 5. As a result, even though `us_east_tablespace` has been created successfully, a notice is displayed after the execution stating that the additional two replicas are to be placed at the YB-Load balancer’s discretion in  `aws.us-east.us-east-1a`, `aws.us-east.us-east-1b`, and `aws.us-east.us-east-1c`. Based on the load, `aws.us-east.us-east-1a` may have three replicas and the other zones may have one, or it might later change such that `aws.us-east.us-east-1a` and `aws.us-east.us-east-1b` have two replicas each, whereas `aws.us-east.us-east-1c` has one replica. YB-Load balancer always honours the value of `min_num_replicas` and have at least one replica in each cloud.region.zone, and the additional replicas may be moved around based on the cluster load. 

Using the `us_west_tablespace` and `us_east_tablespace`, you can apply the YSQL's `TABLESPACE` option to `CREATE TABLE` and `CREATE INDEX` statements, as follows:

```sql
CREATE TABLE employees (
  employee_no integer PRIMARY KEY,
  name text,
  department text,
  change_date date
)
TABLESPACE us_west_tablespace;
```

```sql
CREATE INDEX employee_no_idx ON employees(employee_no) 
TABLESPACE us_east_tablespace;
```

The preceding statements ensure that data in the `employees` and `employee_no_idx` tables is present based on the placement policies specified for `us_west_tablespace` and `us_east_tablespace` respectively.

## Dropping Tablespaces

You can drop a tablespace in YSQL using the following syntax:

```sql
DROP TABLESPACE tablespace_name;
```

The following example shows how to drop one of the tablespaces created in [Multi-Zone Tablespaces](multi-zone-tablespaces):

```sql
DROP TABLESPACE us_west_tablespace;
```

Note that the preceding operation cannot be performed if the tablespace has associated tables or indexes.

## Limitations

The current release of YugabyteDB has the following limitations related to tablespaces:

- `ALTER TABLE` in combination with `SET TABLESPACE` is not supported.
- `ALTER TABLESPACE` is not supported.
- Setting read replica placements and affinitized leaders using tablespaces is not supported.
- Setting tablespaces for colocated tables and databases is not supported.

