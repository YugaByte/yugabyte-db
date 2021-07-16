---
title: Point-in-Time Recovery
headerTitle: Point-in-time recovery
linkTitle: Point-in-time recovery
description: Restore data from a specific point in time in YugabyteDB
aliases:
- /latest/manage/backup-restore/point-in-time-restore
- /latest/manage/backup-restore/point-in-time-restore-ysql
- /latest/manage/backup-restore/point-in-time-restore-ycql
menu:
  latest:
    identifier: point-in-time-recovery
    parent: backup-restore
    weight: 705
isTocNested: true
showAsideToc: true
---

_Point-in-time recovery_ (also referred to here as PITR) helps in recovering from a number of error or failure scenarios, by allowing the database to be restored to a specific point in time in the past.

PITR depends on _full backups_ (also referred to as base backups). A full backup, as the name suggests, is a complete transactional backup of data up to a certain point in time. The entire data set in the database is backed up for all of the namespaces and tables you selected. Full backups are resource-intensive, and can consume considerable amounts of CPU time, bandwidth, and disk space.

To learn more about YugabyteDB's point-in-time recovery feature, refer to the [Recovery scenarios](#recovery-scenarios), [Features](#features), [Use cases](#use-cases), and [Limitations](#limitations) sections on this page. For more details on the `yb-admin` commands, refer to the [Backup and snapshot commands](../../../admin/yb-admin#backup-and-snapshot-commands) section of the yb-admin documentation. For information on how to setup PITR in production, refer to [Production recommendations](#production-recommendations)

## Try out the PITR feature

There are several recovery scenarios [for YSQL](../../../explore/backup-restore/point-in-time-recovery-ysql/) and [for YCQL](../../../explore/backup-restore/point-in-time-recovery-ycql/) in the Explore section.

## Use cases

The following table provides a quick comparison of the intended usage patterns.

| Scenario | In-cluster flashback DB | Off-cluster flashback DB |
| :------- | :---------------------- | :----------------------- | :----------------- |
| **Disk/file corruption** | Handled by replication in cluster | Handled by replication in cluster |
| **App/operator error** | Yes | Yes |
| **RPO** | Very low | High |
| **RTO** | Very low | High |
| **Disaster Recovery** | No (replication in cluster) | Yes |
| **Impact / Cost** | Very low | High (snapshot and copy) |

## Recovery scenarios

### App and operator errors

Point in time recovery allows recovery from the following scenarios by restoring the database to a point in time before the error occurred. The errors could be any of the following:

* DDL errors: For example, a table is dropped by mistake
* DML errors: For example, an erroneous UPDATE statement is run on the table

In both cases, you restore the table to a point in time before the error occurred.

### Disk or filesystem corruption

Data loss can happen due to one of the following reasons:

* Loss or failure of a disk
* Deletion of DB data files; for example, through operator error
* Bugs in the database software; for example, due to a software upgrade

In a distributed SQL database such as YugabyteDB, the first two scenarios can be mitigated due to the presence of live replicas, as it's highly unlikely the same issue occurs on all nodes. However, for the third scenario, PITR is an important solution.

### Disasters

This is the scenario in which the data in the entire source cluster is lost irrecoverably, and a restore needs to be performed from a remote location. While the likelihood of this scenario is low, it's still important to understand the probability of correlated failures. For example, loss due to a natural disaster has a very low probability of occurrence in a multi-region deployment, but its probability increases with the proximity of the replicas.

## Production recommendations

Configuring PITR has to go hand in hand with configuring your backup schedules. [TBD some more info on why external backups are your disaster recovery safeguard]

Let's assume that your backup strategy only allows for taking full backups every 24 hours. Then you would configure your cluster for PITR as follows:
- Set history retention (LINK TBD) to 25 hours.Your external backup data needs to have all 24 hours worth of updates for every key, so you can safely use PITR on it (this is inline with YugabyteDB's MVCC data model). The retention should be slightly more than your 24 hours backup schedule, to allow for some jitter, if backups are not taken exactly on time.
- Set snapshot schedule interval (LINK TBD) to the same 24 hours as your backup schedule.
- Set snapshot schedule retention (LINK TBD) to 25 hours. This should be something longer than your backup schedule of 24 hours, for the same jitter reasons as the history retention.

With this configuration, depending on the timestamp you wish to restore to, you have two options:
- A time between now and 24 hours ago: use the restore_snapshot_schedule command to operate on the in-cluster data.
- A time older than 24 hours ago: need to recover from an external backup.

For how to configure PITR, refer to the yb-admin pages (TBD).

## Limitations

This feature is in active development. YSQL and YCQL support different features, as detailed in the sections that follow.

A generic limitation across both APIs is the ability to use PITR on data from an external backup. Support for data only is tracked in [issue 8846](https://github.com/yugabyte/yugabyte-db/issues/8846). Support for metadata is tracked in [issue 8847](https://github.com/yugabyte/yugabyte-db/issues/8847).

### YSQL limitations

Currently, you can **restore data only**. The feature doesn't support metadata; in other words, restoring past operations such as CREATE, ALTER, TRUNCATE, and DROP TABLE is unsupported.

Development for this feature is tracked in [issue 7120](https://github.com/yugabyte/yugabyte-db/issues/7120). Support for undoing certain metadata operations is forthcoming.

### YCQL limitations

Currently, you can recover from the following YCQL operations:

* Data changes
* CREATE and DROP TABLE
* ALTER TABLE (including ADD, DROP, and RENAME COLUMN)
* CREATE and DROP INDEX

Development for this feature is tracked in [issue 7120](https://github.com/yugabyte/yugabyte-db/issues/7120). Some forthcoming features include:

* YCQL roles and permissions
* Support for automatic tablet splitting
* Support for TRUNCATE
