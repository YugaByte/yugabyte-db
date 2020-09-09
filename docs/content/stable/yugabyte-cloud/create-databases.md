---
title: Create databases
linkTitle: Create databases
description: Create databases
headcontent:
image: /images/section_icons/deploy/enterprise.png
beta: /stable/faq/general/#what-is-the-definition-of-the-beta-feature-tag
aliases:
  - /stable/deploy/yugabyte-cloud/create-databases/
block_indexing: true
menu:
  stable:
    identifier: create-databases
    parent: yugabyte-cloud
    weight: 643
isTocNested: true
showAsideToc: true
---

## Create a database using YSQL

You can use the YSQL shell (`ysqlsh`) and the [YSQL API](../../api/ysql/) to create and manage YugabyteDB databases and tables in your Yugabyte Cloud clusters.

To configure a local YSQL shell to connect to your clusters, see [Connect using the YSQL shell (ysqlsh)](../connect-to-clusters/#connect-using-the-ysql-shell-ysqlsh).

## Create a keyspace using YCQL

You can use the YCQL shell (`ycqlsh`) and the [YCQL API](../../api/ycql/) to create and manage YugabyteDB keyspaces and tables in your Yugabyte Cloud clusters.

To configure a local YCQL shell to connect to your clusters, see [Connect using the YCQL shell (cqlsh)](../connect-to-clusters/#connect-using-the-ycql-shell-cqlsh).

## Create a database using a sample dataset

[Sample datasets](../../sample-data/) are available to help you learn and explore YugabyteDB. The SQL script files (`.sql`) required to create
sample databases are included in the `share` directory of the YugabyteDB home directory.

Here are links to documentation on the tested datasets and the steps to create the sample databases:

- [Northwind](../../sample-data/northwind/)
- [PgExercises](../../sample-data/pgexercises/)
- [SportsDB](../../sample-data/sportsdb/)
- [Chinook](../../sample-data/chinook/)
