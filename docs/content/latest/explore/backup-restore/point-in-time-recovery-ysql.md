---
title: Point-in-Time Recovery for YSQL
headerTitle: Point-in-time recovery
linkTitle: Point-in-time recovery
description: Restore data from a specific point in time in YugabyteDB for YSQL
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
aliases:
- /latest/explore/backup-restore/point-in-time-restore-ysql
menu:
  latest:
    identifier: explore-point-in-time-recovery
    parent: explore
    weight: 704
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/explore/backup-restore/point-in-time-recovery-ysql" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      YSQL
    </a>
  </li>
  <li >
    <a href="/latest/explore/backup-restore/point-in-time-recovery-ycql" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      YCQL
    </a>
  </li>
</ul>

The point-in-time recovery feature allows you to restore the state of your cluster's data from a specific point in time. This can be relative, such as "three hours ago", or an absolute timestamp.

Refer to [Features](../../../manage/backup-restore/point-in-time-recovery/#features), [Use cases](../../../manage/backup-restore/point-in-time-recovery/#use-cases), and [Limitations](../../../manage/backup-restore/point-in-time-recovery/#limitations) for details on this feature. For more details on the `yb-admin` commands, refer to the [Backup and snapshot commands](../../../admin/yb-admin/#backup-and-snapshot-commands) section of the yb-admin documentation.

You can try out the PITR feature by creating a database and populating it, creating a snapshot schedule, and restoring ([data only](../../../manage/backup-restore/point-in-time-recovery/#ysql-limitations)!) from a snapshot on the schedule.

{{< tip title="Examples are simplified" >}}

The examples on this page are deliberately simple. In many of the scenarios presented, you could drop the index or table to recover. Consider the examples as part of an effort to undo a larger schema change, such as a database migration, which has performed several operations.

{{< /tip >}}

## Undo data changes

### Create and snapshot a table

Create and populate a table, look at a timestamp to which you'll restore, and then write a row.

1. Start the YSQL shell and connect to your local instance:

    ```sh
    $ bin/ysqlsh -h 127.0.0.1
    ```

1. Create a table and populate some sample data:

    ```sql
    CREATE TABLE employees (
      employee_no integer PRIMARY KEY,
      name text,
      department text,
      salary integer
    );

    INSERT INTO employees (employee_no, name, department, salary) 
      VALUES 
      (1221, 'John Smith', 'Marketing', 50000),
      (1222, 'Bette Davis', 'Sales', 55000),
      (1223, 'Lucille Ball', 'Operations', 70000),
      (1224, 'John Zimmerman', 'Sales', 60000);
    
    SELECT * from employees;
    ```

    ```output
     employee_no |      name      | department | salary 
    -------------+----------------+------------+--------
            1223 | Lucille Ball   | Operations |  70000
            1224 | John Zimmerman | Sales      |  60000
            1221 | John Smith     | Marketing  |  50000
            1222 | Bette Davis    | Sales      |  55000
    (4 rows)
    ```

1. At a terminal prompt, create a snapshot schedule for the database from a shell prompt. In this example, the schedule is one snapshot every minute, and each snapshot is retained for ten minutes.

    ```sh
    $ bin/yb-admin create_snapshot_schedule 1 10 ysql.yugabyte
    ```

    ```output
    {
        "schedule_id": "0e4ceb83-fe3d-43da-83c3-013a8ef592ca"
    }
    ```

1. Verify that a snapshot has happened:

    ```sh
    $ bin/yb-admin list_snapshot_schedules
    ```

    ```output
    {
        "schedules": [
            {
                "id": "0e4ceb83-fe3d-43da-83c3-013a8ef592ca",
                "options": {
                    "interval": "60.000s",
                    "retention": "600.000s"
                },
                "snapshots": [
                    {
                        "id": "8d588cb7-13f2-4bda-b584-e9be47a144c5",
                        "snapshot_time_utc": "2021-05-07T20:16:08.492330+0000"
                    }
                ]
            }
        ]
    }
    ```

### Restore from an absolute time

1. From a command prompt, get a timestamp.

    ```sh
    $ python -c 'import datetime; print datetime.datetime.now().strftime("%s%f")'
    ```

    ```output
    1620418817729963
    ```

1. Add a row for employee 9999 to the table:

    ```sql
    INSERT INTO employees (employee_no, name, department, salary) 
      VALUES 
      (9999, 'Wrong Name', 'Marketing', 10000);

    SELECT * FROM employees;
    ```

    ```output
     employee_no |      name      | department | salary 
    -------------+----------------+------------+--------
            1223 | Lucille Ball   | Operations |  70000
            9999 | Wrong Name     | Marketing  |  10000
            1224 | John Zimmerman | Sales      |  60000
            1221 | John Smith     | Marketing  |  50000
            1222 | Bette Davis    | Sales      |  55000
    (5 rows)
    ```

1. Restore the snapshot schedule to the timestamp you obtained before you added the data, at a terminal prompt.

    ```sh
    $ bin/yb-admin restore_snapshot_schedule 0e4ceb83-fe3d-43da-83c3-013a8ef592ca 1617670679185100
    ```

    ```output
    {
        "snapshot_id": "2287921b-1cf9-4bbc-ad38-e309f86f72e9",
        "restoration_id": "1c5ef7c3-a33a-46b5-a64e-3fa0c72709eb"
    }
    ```

1. Next, verify the restoration is in `RESTORED` state (you'll see more snapshots in the list, as well):

    ```sh
    $ bin/yb-admin list_snapshots
    ```

    ```output
    Snapshot UUID                         State
    8d588cb7-13f2-4bda-b584-e9be47a144c5  COMPLETE
    1f4db0e2-0706-45db-b157-e577702a648a  COMPLETE
    b91c734b-5c57-4276-851e-f982bee73322  COMPLETE
    04fc6f05-8775-4b43-afbd-7a11266da110  COMPLETE
    e7bc7b48-351b-4713-b46b-dd3c9c028a79  COMPLETE
    2287921b-1cf9-4bbc-ad38-e309f86f72e9  COMPLETE
    97aa2968-6b56-40ce-b2c5-87d2e54e9786  COMPLETE
    Restoration UUID                      State
    1c5ef7c3-a33a-46b5-a64e-3fa0c72709eb  RESTORED
    ```

1. In the YSQL shell, verify the data is restored, without a row for employee 9999:

    ```sql
    yugabyte=# select * from employees;
    ```

    ```output
     employee_no |      name      | department | salary 
    -------------+----------------+------------+--------
            1223 | Lucille Ball   | Operations |  70000
            1224 | John Zimmerman | Sales      |  60000
            1221 | John Smith     | Marketing  |  50000
            1222 | Bette Davis    | Sales      |  55000
    (4 rows)
    ```

### Restore from a relative time

In addition to restoring to a particular timestamp, you can also restore from a relative time, such as "ten minutes ago".

When you specify a relative time, you can specify any or all of _days_, _hours_, _minutes_, and _seconds_. For example:

* `"5m"` to restore from five minutes ago
* `"1h"` to restore from one hour ago
* `"3d"` to restore from three days ago
* `"1h 5m"` to restore from one hour and five minutes ago

Relative times can be in any of the following formats (again, note that you can specify any or all of days, hours, minutes, and seconds):

* ISO 8601: `3d 4h 5m 6s`
* Abbreviated PostgreSQL: `3 d 4 hrs 5 mins 6 secs`
* Traditional PostgreSQL: `3 days 4 hours 5 minutes 6 seconds`
* SQL standard: `D H:M:S`

**Careful!** If you specify a time prior to when you created the table, the restore will leave the table intact, but empty.

Refer to the yb-admin [_restore_snapshot_schedule_ command](../../../admin/yb-admin/#restore-snapshot-schedule) for more details.

## Delete snapshot schedules

To delete a snapshot schedule, use the yb-admin [_delete_snapshot_schedule_ command](../../../admin/yb-admin/#delete-snapshot-schedule). For example:

```sh
$ ./bin/yb-admin \
    delete_snapshot_schedule 0002bcb0-bffd-4729-9624-c18ac7797f57
```

```output
{
    "schedule_id": "0002bcb0-bffd-4729-9624-c18ac7797f57"
}
```

Note that it can take a few seconds for the schedule to be deleted, so the schedule may appear in the output of `list_snapshot_schedules` during that time.
