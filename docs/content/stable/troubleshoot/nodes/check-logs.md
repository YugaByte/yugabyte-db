---
title: Inspect logs
linkTitle: Inspect logs
headerTitle: Inspect YugabyteDB logs
description: Inspect YugabyteDB logs
aliases:
  - /troubleshoot/nodes/check-logs/
block_indexing: true
menu:
  stable:
    parent: troubleshoot-nodes
    weight: 844
isTocNested: true
showAsideToc: true
---

## YugabyteDB base folder

The logs for each node are written to a subdirectory of the YugabyteDB `yugabyte-data` directory and may vary depending on your deployment:

- When you use `yb-ctl` to create local YugabyteDB clusters on a single host (for example, your laptop), the default location for each node is `/yugabyte-data/node-<node_nr>/`. 
For a 3-node cluster, the `yb-ctl` utility creates three directories: `node-1`, `node-2` and `node-3`.
- For a multi-node cluster deployment to multiple hosts, the location where YugabyteDB disks are set up can vary (for example, `/home/centos/`, `/mnt/`, or another directory) on each node (host).
- When using the `--fs_data_dirs` flag with multiple directories, logs will be saved in the first directory in the list.
- When using Yugabyte Platform, logs are located in `/home/yugabyte/{master,tserver}/logs`. This is a symlink to the first directory in `--fs_data_dirs` list.

In the sections below, the YugabyteDB `yugabyte-data` directory is represented by `<yugabyte-data-directory>`.

## YB-Master logs

The YB-Master service manages system metadata, such as namespaces (databases or keyspaces) and tables. It also handles DDL statements such as `CREATE TABLE`, `DROP TABLE`, `ALTER TABLE` / `KEYSPACE/TYPE`.  It also manages users, permissions, and coordinate background operations, such as load balancing. Its logs can be found at:

```sh
$ cd <yugabyte-data-directory>/disk1/yb-data/master/logs/
```

Logs are organized by error severity: `FATAL`, `ERROR`, `WARNING`, `INFO`. In case of issues, the `FATAL` and `ERROR` logs are most likely to be relevant.

## YB-TServer logs

The YB-TServer service performs the actual I/O for end-user requests. It handles DML statements such as `INSERT`, `UPDATE`, `DELETE`, and `SELECT`. Its logs can be found at:

```sh
$ cd <yugabyte-data-directory>/disk1/yb-data/tserver/logs/
```

Logs are organized by error severity: `FATAL`, `ERROR`, `WARNING`, `INFO`. In case of issues, the `FATAL` and `ERROR` logs are most likely to be relevant.

## Logs management

There are 3 types of logs:

For YB-Master and YB-TServer, the log rotation size is controlled by the `--max_log_size` flag.

`--max_log_size=256` will limit each file to 256MB. The default size is 1.8 GB.

For YSQL, you also have the additional `postgres*log` files. These logs have daily and size-based log rotation, that is a new log file will be created each day or a log reaches 10 MB size.

For available configuration flags, see [YB-Master logging flags](../../../reference/configuration/yb-master/#logging-flags) and [YB-TServer logging flags](../../../reference/configuration/yb-tserver/#logging-flags).
