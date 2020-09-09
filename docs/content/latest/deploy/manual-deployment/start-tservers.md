---
title: Start YB-TServer servers
headerTitle: Start YB-TServers
linkTitle: 4. Start YB-TServers
description: Steps to start YB-TServers when deploying for a single region or data center in a multi-zone/multi-rack configuration.
aliases:
  - /deploy/manual-deployment/start-tservers
menu:
  latest:
    identifier: deploy-manual-deployment-start-tservers
    parent: deploy-manual-deployment
    weight: 614
isTocNested: true
showAsideToc: true
---

{{< note title="Note" >}}

- The number of nodes in a cluster running YB-TServers **must** equal or exceed the replication factor in order for any table to get created successfully.
- For running a single cluster across multiple data centers or 2 clusters in 2 data centers, refer to the [Multi-DC Deployments](../../../deploy/multi-dc/) section.

{{< /note >}}

This section covers deployment for a single region or data center in a multi-zone/multi-rack configuration. Note that single zone configuration is a special case of multi-zone where all placement related flags are set to the same value across every node. 

## Example scenario

- Create a 6-node cluster with replication factor of 3.
      - YB-TServer server should on all the six nodes, but as noted in the previous section, the YB-Master server should run on only three of these nodes.
      - Assume the three YB-Master private IP addresses are `172.151.17.130`, `172.151.17.220` and `172.151.17.140`.
      - Cloud will be `aws`, region will be `us-west` and the 3 AZs will be `us-west-2a`, `us-west-2b`, and `us-west-2c`. Two nodes will be placed in each AZ in such a way that 1 replica for each tablet (aka shard) gets placed in any 1 node for each AZ. 
- Multiple data drives mounted on `/home/centos/disk1`, `/home/centos/disk2`

## Run YB-TServer with command line flags

Run the `yb-tserver` server on each of the six nodes as shown below. Note that all of the master addresses have to be provided using the `--tserver_master_addrs` flag. Replace the [`--rpc_bind_addresses`](../../../reference/configuration/yb-tserver/#rpc-bind-addresses) value with the private IP address of the host as well as the set the `placement_cloud`,`placement_region` and `placement_zone` values appropriately. For single zone deployment, simply use the same value for the `--placement_zone` flag.

```sh
$ ./bin/yb-tserver \
  --tserver_master_addrs 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
  --rpc_bind_addresses 172.151.17.130:9100 \
  --start_pgsql_proxy \
  --pgsql_proxy_bind_address 172.151.17.130:5433 \
  --cql_proxy_bind_address 172.151.17.130:9042 \
  --fs_data_dirs "/home/centos/disk1,/home/centos/disk2" \
  --placement_cloud aws \
  --placement_region us-west \
  --placement_zone us-west-2a \
  >& /home/centos/disk1/yb-tserver.out &
```

For the full list of configuration flags, see the [YB-TServer reference](../../../reference/configuration/yb-tserver/).

{{< note title="Note" >}}

The number of comma-separated values in the [`--tserver_master_addrs`](../../../reference/configuration/yb-tserver/#tserver-master-addrs)) flag should match the total number of YB-Master servers (or the replication factor).

{{< /note >}}

## Run YB-TServer with configuration file

Alternatively, you can also create a `tserver.conf` file with the following flags and then run the `yb-tserver` with the [`--flagfile`](../../../reference/configuration/yb-tserver/#flagfile)) flag as shown here. For each YB-TServer server, replace the RPC bind address flags with the private IP address of the host running the YB-TServer server.

```sh
--tserver_master_addrs=172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100
--rpc_bind_addresses=172.151.17.130:9100
--start_pgsql_proxy
--pgsql_proxy_bind_address=172.151.17.130:5433
--cql_proxy_bind_address=172.151.17.130:9042
--fs_data_dirs=/home/centos/disk1,/home/centos/disk2
--placement_cloud=aws 
--placement_region=us-west 
--placement_zone=us-west-2a 
```

```sh
$ ./bin/yb-tserver --flagfile tserver.conf >& /home/centos/disk1/yb-tserver.out &
```

## Set replica placement policy

{{< note title="Note" >}}

This step is required for only multi-AZ deployments and can be skipped for a single AZ deployment.

{{< /note >}}

The default replica placement policy when the cluster is first created is to treat all nodes as equal irrespective of the `--placement_*` configuration flags.  However, for the current deployment, you want to explicitly place one replica of each tablet in each AZ. The following command sets replication factor of `3` across `us-west-2a`, `us-west-2b`, `us-west-2c` leading to such a placement.

On any host running the yb-master, run the following command.

```sh
$ ./bin/yb-admin \
    --master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
    modify_placement_info  \
    aws.us-west.us-west-2a,aws.us-west.us-west-2b,aws.us-west.us-west-2c 3
```

Verify by running the following.

```sh
$ curl -s http://<any-master-ip>:7000/cluster-config
```

And confirm that the output looks similar to what is shown below with `min_num_replicas` set to `1` for each AZ.

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
        placement_region: "us-west"
        placement_zone: "us-west-2b"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2b"
      }
      min_num_replicas: 1
    }
  }
}
```

## Verify health

Make sure all YB-TServer servers are now working as expected by inspecting the INFO log. The default logs directory is always inside the first directory specified in the [`--fs_data_dirs`](../../../reference/configuration/yb-tserver/#fs-data-dirs) flag.

You can do this as shown below.

```sh
$ cat /home/centos/disk1/yb-data/tserver/logs/yb-tserver.INFO
```

In each of the four YB-TServer logs, you should see log messages similar to the following.

```
I0912 16:27:18.296516  8168 heartbeater.cc:305] Connected to a leader master server at 172.151.17.140:7100
I0912 16:27:18.296794  8168 heartbeater.cc:368] Registering TS with master...
I0912 16:27:18.297732  8168 heartbeater.cc:374] Sending a full tablet report to master...
I0912 16:27:18.298435  8142 client-internal.cc:1112] Reinitialize master addresses from file: ../tserver.conf
I0912 16:27:18.298691  8142 client-internal.cc:1123] New master addresses: 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100
I0912 16:27:18.311367  8142 webserver.cc:156] Starting webserver on 0.0.0.0:12000
I0912 16:27:18.311408  8142 webserver.cc:161] Document root: /home/centos/yugabyte/www
I0912 16:27:18.311574  8142 webserver.cc:248] Webserver started. Bound to: http://0.0.0.0:12000/
I0912 16:27:18.311748  8142 rpc_server.cc:158] RPC server started. Bound to: 0.0.0.0:9042
I0912 16:27:18.311828  8142 tablet_server_main.cc:128] CQL server successfully started
```

In the current YB-Master leader log, you should see log messages similar to the following.

```
I0912 22:26:32.832296  3162 ts_manager.cc:97] Registered new tablet server { permanent_uuid: "766ec935738f4ae89e5ff3ae26c66651" instance_seqno: 1505255192814357 } with Master
I0912 22:26:39.111896  3162 ts_manager.cc:97] Registered new tablet server { permanent_uuid: "9de074ac78a0440c8fb6899e0219466f" instance_seqno: 1505255199069498 } with Master
I0912 22:26:41.055996  3162 ts_manager.cc:97] Registered new tablet server { permanent_uuid: "60042249ad9e45b5a5d90f10fc2320dc" instance_seqno: 1505255201010923 } with Master
```

{{< tip title="Tip" >}}

Remember to add the command you used to start the YB-TServer to a `cron` job to restart it if it goes down.

{{< /tip >}}
