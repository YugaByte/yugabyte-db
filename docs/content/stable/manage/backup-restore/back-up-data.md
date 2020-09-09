---
title: Back up data for YSQL
headerTitle: Back up data
linkTitle: Back up data
description: Back up data in YugabyteDB for YSQL.
aliases:
  - /manage/backup-restore/backing-up-data
block_indexing: true
menu:
  stable:
    identifier: back-up-data
    parent: backup-restore
    weight: 703
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/stable/manage/backup-restore/back-up-data" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/stable/manage/backup-restore/back-up-data-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

YugabyteDB is designed for reliability, providing high fault tolerance and redundancy. Many YSQL applications do not require backups, except for disaster recovery needs or to create YugabyteDB clusters for development and testing. When you need backups, YugabyteDB provides two utilities, `ysql_dump` and `ysql_dumpall`, to back up databases.

## Back up a single database

The YugabyteDB [`ysql_dump`](../../../admin/ysql-dump) backup utility, derived from PostgreSQL `pg_dump`,
can be used to extract a single YugabyteDB database into a SQL script file.  `ysql_dump` will make a consistent backup for a database, even if it is being used concurrently, and does not block other database users (readers or writers).

The SQL script dump is a plain-text file that include the SQL statements required to restore the database to the state it was in at the time it was saved. To restore a database from the SQL script file, use [`ysqlsh`](../../../admin/ysqlsh). For details on restoring an individual YugabyteDB database, see [Restore data](../restore-data).

To back up all of the databases in a YugabyteDB universe or cluster, including metadata (roles, sequences, and other database objects), use the `ysql_dumpall` utility.

To back up a single database, from your YugabyteDB home directory, run the `ysql_dump` command, specifying the database to be backed up.

```sh
$ ./postgres/bin/ysql_dump -d <db-name> > <backup-file>
```

- *db-name*: The name of the database to be backed up.
- *backup-file*: The path to the backup file.

In the following example, the database `mydb` is extracted to the backup file `mydb-dump.sql`, located in a backup directory.

```sh
$ ./postgres/bin/ysql_dump -d mydb > ../backup/mydb-dump.sql
```

For details on this utility and the optional parameters, see [`ysql_dump`](../../../admin/ysql-dump).

## Back up all databases

Use the [`ysql_dumpall`](../../../admin/ysql-dumpall) backup utility to write out (or "dump") all YugabyteDB databases of a universe into a single SQL script file. The script file is a plain-text file that contains SQL statements that can be used as input to `ysqlsh` to restore the databases. `ysql_dumpall` uses calls to `ysql_dump` for each database in the universe. Unlike the `ysql_dump` utility, the `ysql_dumpall` utility backs up all database objects, including roles, databases, schemas, tables, indexes, triggers, functions, constraints, ownerships, and privileges.

{{< note title="Note" >}}

*If you have enabled password authentication*, you will be prompted for a password as each database in your YugabyteDB universe is backed up because `ysql_dumpall` connects once per database. To have your backup proceed uninterrupted, you can use a password file (`~/.pgpass`).

{{< /note >}}

To back up all databases, from your YugabyteDB home directory, run the [`ysql_dumpall`](../../../admin/ysql-dumpall) utility command.

```sh
$ ./postgres/bin/ysql_dumpall > <backup-file>
```

- *backup-file*: The path to the backup file.

In the following example, all databases and database objects are backed up to the specified file.

```sh
$ ./postgres/bin/ysql_dumpall > ../backup/yb-dumpall.sql
```

Options for the `ysql_dumpall` utility can be used to limit backups to roles only (`--roles-only`), all database objects except the data (`--schema-only`), or other options. For details about the utility's available options, see [`ysql_dumpall`](../../../admin/ysql-dumpall).
