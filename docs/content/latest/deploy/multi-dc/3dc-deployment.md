---
title: Deploy to three or more data centers
headerTitle: Three+ data center (3DC)
linkTitle: Three+ data center (3DC)
description: Deploy YugabyteDB clusters to three or more data centers.
menu:
  latest:
    parent: multi-dc
    identifier: 3dc-deployment
    weight: 632
type: page
isTocNested: true
showAsideToc: true
---

{{< tip title="Recommended Reading" >}}

[9 Techniques to Build Cloud-Native, Geo-Distributed SQL Apps with Low Latency](https://blog.yugabyte.com/9-techniques-to-build-cloud-native-geo-distributed-sql-apps-with-low-latency/) highlights the various multi-DC deployment strategies (including 3DC deployments) for a distributed SQL database like YugabyteDB. 

{{< /tip >}}

Three data center deployments of YugabyteDB are essentially a natural extension of the three availability zone (AZ) deployments documented in the [Manual deployment](../../manual-deployment/) section. Equal number of nodes are now placed in each data center of the three data centers. Inside a single data center, a multi-AZ deployment is recommended to ensure resilience against zone failures. This approach works fine for any odd number of AZs or data centers. Given YugabyteDB's distributed consensus-based replication which requires majority quorum for continuous availability of write requests, deploying a single cluster across an even number of AZs or data centers is not recommended. 

## Example scenario

- Create a three-node cluster with replication factor of `3`.
      - Cloud will be `aws` and the three regions/AZs will be `us-west`/`us-west-2a`, `us-east-1`/`us-east-1a`, `ap-northeast-1`/`ap-northeast-1a`. One node will be placed in each region/AZ in such a way that one replica for each tablet also gets placed in each region/AZ.
      - Private IP addresses of the 3 nodes are `172.151.17.130`, `172.151.17.220`, and `172.151.17.140`.
- We have multiple data drives mounted on `/home/centos/disk1`, `/home/centos/disk2`.

## Prerequisites

Follow the [Checklist](../../../deploy/checklist/) to ensure you have prepared the nodes for installing YugabyteDB.

Execute the following steps on each of the instances.

## 1. Install software

Follow the [installation instructions](../../../deploy/manual-deployment/install-software) to install YugabyteDB on each of the nodes.

## 2. Start YB-Masters

Run the [yb-master](../../../reference/configuration/yb-master/) server on each of the nodes as shown below. Note how multiple directories can be provided to the `--fs_data_dirs` flag. Replace the [`--rpc_bind_addresses`](../../../reference/configuration/yb-master/#rpc-bind-addresses) value with the private IP address of the host as well as the set the `--placement_cloud`,`--placement_region` and `--placement_zone` values appropriately.

```sh
$ ./bin/yb-master \
  --master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
  --rpc_bind_addresses 172.151.17.130 \
  --fs_data_dirs "/home/centos/disk1,/home/centos/disk2" \
  --placement_cloud aws \
  --placement_region us-west \
  --placement_zone us-west-2a \
  --leader_failure_max_missed_heartbeat_periods 10 \
  >& /home/centos/disk1/yb-master.out &
```

Note that you also set the [`--leader_failure_max_missed_heartbeat_periods`](../../../reference/configuration/yb-master/#leader-failure-max-missed-heartbeat-periods) flag to `10`. This flag specifies the maximum heartbeat periods that the leader can fail to heartbeat before the leader is considered to be failed. Since the data is geo-replicated across data centers, RPC latencies are expected to be higher. We use this flag to increase the failure detection interval in such a higher RPC latency deployment. Note that the total failure timeout is now 5 seconds since it is computed by multiplying [`--raft_heartbeat_interval_ms`](../../../reference/configuration/yb-master/#raft-heartbeat-interval-ms) (default of 500ms) with [`--leader_failure_max_missed_heartbeat_periods`](../../../reference/configuration/yb-master/#leader-failure-max-missed-heartbeat-periods)(current value of `10`).

For the full list of configuration flags, see the [YB-Master reference](../../../reference/configuration/yb-master/).

## 3. Start YB-TServers

Run the [yb-tserver](../../../reference/configuration/yb-tserver/) server on each node as shown below . Note that all of the master addresses have to be provided using the [`--tserver_master_addrs`](../../../reference/configuration/yb-master/#tserver-master-addrs) flag. Replace the [`--rpc_bind_addresses`](../../../reference/configuration/yb-tserver/#rpc-bind-addresses) value with the private IP address of the host as well as the set the `placement_cloud`,`placement_region` and `placement_zone` values appropriately.

```sh
$ ./bin/yb-tserver \
  --tserver_master_addrs 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
  --rpc_bind_addresses 172.151.17.130 \
  --start_pgsql_proxy \
  --pgsql_proxy_bind_address 172.151.17.130:5433 \
  --cql_proxy_bind_address 172.151.17.130:9042 \
  --fs_data_dirs "/home/centos/disk1,/home/centos/disk2" \
  --placement_cloud aws \
  --placement_region us-west \
  --placement_zone us-west-2a \
  --leader_failure_max_missed_heartbeat_periods 10 \
  >& /home/centos/disk1/yb-tserver.out &
```

Note that you also set the [`--leader_failure_max_missed_heartbeat_periods`](../../../reference/configuration/yb-tserver/#leader-failure-max-missed-heartbeat-periods) flag to `10`. This flag specifies the maximum heartbeat periods that the leader can fail to heartbeat before the leader is considered to be failed. Since the data is geo-replicated across data centers, RPC latencies are expected to be higher. We use this flag to increase the failure detection interval in such a higher RPC latency deployment. Note that the total failure timeout is now 5 seconds since it is computed by multiplying [--raft_heartbeat_interval_ms](../../../reference/configuration/yb-tserver/#raft-heartbeat-interval-ms) (default of 500ms) with leader_failure_max_missed_heartbeat_periods (current value of `10`).

For the full list of configuration flags, see the [YB-TServer reference](../../../reference/configuration/yb-tserver/).

## 4. Set replica placement policy

The default replica placement policy when the cluster is first created is to treat all nodes as equal irrespective of the `--placement_*` configuration flags.  However, for the current deployment, you want to explicitly place one replica of each tablet in each region/AZ. The following command sets replication factor of `3` across `us-west-2`/`us-west-2a`, `us-east-1`/`us-east-1a`, `ap-northeast-1`/`ap-northeast-1a` leading to such a placement.

On any host running the yb-master, run the following command.

```sh
$ ./bin/yb-admin \
    --master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
    modify_placement_info  \
    aws.us-west.us-west-2a,aws.us-east-1.us-east-1a,aws.ap-northeast-1.ap-northeast-1a 3
```

Verify by running the following.

```sh
$ curl -s http://<any-master-ip>:7000/cluster-config
```

And confirm that the output looks similar to what is shown below with [`--min_num_replicas`](../../../reference/configuration/yb-tserver/#min-num-replicas) set to `1` for each AZ.

```
replication_info {
  live_replicas {
    num_replicas: 3
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2a"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-east-1"
        placement_zone: "us-east-1a"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "ap-northeast-1"
        placement_zone: "ap-northeast-1a"
      }
      min_num_replicas: 1
    }
  }
}
```

One additional option to consider is to set a preferred location for all the tablet leaders using the [yb-admin set_preferred_zones](../../../admin/yb-admin#set-preferred-zones) command. 
For multi-row or multi-table transactional operations, colocating the leaders within a single zone or region can help reduce the number 
of cross-region network hops involved in executing a transaction and, as a result, improve performance.

The following command sets the preferred zone to `aws.us-west.us-west-2a`:

```sh
$ ./bin/yb-admin \
    --master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
    set_preferred_zones  \
    aws.us-west.us-west-2a
```

Looking again at the cluster configuration, you should see `affinitized_leaders` added:


```
replication_info {
  live_replicas {
    num_replicas: 3
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2a"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-east-1"
        placement_zone: "us-east-1a"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "ap-northeast-1"
        placement_zone: "ap-northeast-1a"
      }
      min_num_replicas: 1
    }
  affinitized_leaders {
    placement_cloud: "aws"
    placement_region: "us-west"
    placement_zone: "us-west-2a"
  }
  }
}
```



## 5. Verify deployment

Use [`ysqlsh`](../../../admin/ysqlsh/) (for YSQL API) or [`ycqlsh`](../../../admin/cqlsh/) (for YCQL API) shells to test connectivity to the cluster.
