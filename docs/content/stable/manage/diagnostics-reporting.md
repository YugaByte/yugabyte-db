---
title: Diagnostics reporting
headerTitle: Diagnostics reporting
linkTitle: Diagnostics reporting
description: Enable diagnostics reporting and set collection levels on YB-Master and YB-TServer nodes.
aliases:
  - /stable/manage/diagnostics-reporting/
block_indexing: true
menu:
  stable:
    identifier: diagnostics-reporting
    parent: manage
    weight: 706
isTocNested: true
showAsideToc: true
---

The [YB-Master](../../reference/configuration/yb-master/) and [YB-TServer](../../reference/configuration/yb-tserver/) nodes report cluster diagnostics to the YugabyteDB diagnostics service every time a new cluster gets created and every hour thereafter. User data stored in YugabyteDB as well as any personally identifiable information regarding the user is never collected or reported.

## Data collected

The data collected depends on the collection level set. See [Configuration flags](#configuration-flags) below on how to change the collection level.

### Collection level: low

```
Collection time
Cluster uuid
Node uuid
Node type (tserver or master)
Number of masters (yb-master processes)
Number of tablet servers (yb-tserver processes)
Number of tables
Number of tablets
Cluster configuration (gflags)
Host name (for each node where yb-master and yb-tserver processes are running)
Username (for each node where yb-master and yb-tserver processes are running)
```

### Collection level: medium [Default]

```
Everything we collect for level “low” plus:
Performance metrics
RPC metrics
```

### Collection level: high

Same as `medium` level. In other words, no extra information is collected for this level. It will be used in the future to collect additional diagnostics information such as error logs.

#### Example of metrics collected

```
[
    {
        "type": "tablet",
        "id": "eca8b3cfb2ee4eca94dde519634e1e38",
        "attributes": {
            "partition": "hash_split: [16380, 19110)",
            "table_name": "redis",
            "table_id": "82f5f7ab81a44923b5f544fbd0664afe"
        },
        "metrics": [
            {
                "name": "log_reader_bytes_read",
                "value": 0
            },
            {
                "name": "log_reader_entries_read",
                "value": 0
            },
            {
                "name": "log_reader_read_batch_latency",
                "total_count": 0,
                "min": 0,
                "mean": 0,
                "percentile_75": 0,
                "percentile_95": 0,
                "percentile_99": 0,
                "percentile_99_9": 0,
                "percentile_99_99": 0,
                "max": 0,
                "total_sum": 0
            }
         ]
    }
}
```

#### Example of RPCs being collected

```
{
    "inbound_connections": [
        {
            "remote_ip": "10.150.0.20:41134",
            "state": "OPEN",
            "remote_user_credentials": "{real_user=yugabyte, eff_user=}",
            "processed_call_count": 2456
        },
        {
            "remote_ip": "10.150.0.20:54141",
            "state": "OPEN",
            "remote_user_credentials": "{real_user=yugabyte, eff_user=}",
            "processed_call_count": 2471
        }
    ]
}
```

## Configuration flags

You can add the following configuration flags while starting the [YB-Master](../../reference/configuration/yb-master/) and [YB-TServer](../../reference/configuration/yb-tserver/) nodes to configure the diagnostics reporting behavior of YugabyteDB.

Flag | Default | Description
----------------------|---------|------------------------
`--callhome_collection_level` |  `medium` | Collection level with possible values of `low`, `medium`, or `high`
`--callhome_interval_secs` | 3600 | Collection interval in seconds
`--callhome_url ` | `http://diagnostics.yugabyte.com` | Endpoint where diagnostics information is reported
`--callhome_enabled` | `true` | Controls whether diagnostics information is collected and reported. Set to `false` to disable collection.
