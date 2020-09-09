---
title: Restore data for YSQL
headerTitle: Restore data
linkTitle: Restore data
description: Restore data in YugabyteDB for YSQL
block_indexing: true
menu:
  v2.1:
    identifier: restore-data
    parent: backup-restore
    weight: 703
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/manage/backup-restore/restore-data" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/latest/manage/backup-restore/restore-data-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

To restore your YugabyteDB databases that were backed up using the `ysql_dump` and `ysql_dumpall` backup utilities, you can use the [`ysqlsh`](../../../admin/ysqlsh) to restore one or all databases.

## Restore databases using `ysqlsh \i`

To restore databases using the `ysqlsh` shell:

1. Open the `ysqlsh` from the YugabyteDB home directory.

```sh
$ ./bin/ysqlsh
```

```
ysqlsh (11.2-YB-2.0.9.0-b0)
Type "help" for help.

yugabyte=#
```

2. At the `ysqlsh` shell prompt, run the following `\i` command.

```postgresql
yugabyte=# \i <db-sql-script>
```

- *db-sql-script*: the SQL script file dumped using `ysql_dump` or `ysql_dumpall`.

For details about using this command, see [`\i`](../../../admin/ysqlsh/#-i-filename-include-filename) in the [`ysqlsh`](../../../admin/ysqlsh) documentation.

{{< note title="Note" >}}

To reduce security risks, SQL script files created using `ysql_dump` include `SELECT pg_catalog.set_config('search_path', '', false);` to set the `search_path` to an empty string. As a result, when you restore a database from the `ysqlsh` shell, running the `\d` command returns "Did not find any relations." To see the restored database, set the `search_path` again to expected schema to see the restored database.

{{< /note >}}

## Restore databases using `ysqlsh -f`

To restore one database using `ysqlsh` on the command line, open a command line window and run the following command. The `ysqlsh -f` command will read the SQL statements from the specified file and 

```sh
$ ysqlsh -f <db-sql-script>
```

- *db-sql-script*: the SQL script file dumped using `ysql_dump` or `ysql_dumpall`.

For details about using this command, see [`-f, -file`](../../../admin/ysqlsh/#-f-filename-file-filename) in the [`ysqlsh`](../../../admin/ysqlsh) documentation.
