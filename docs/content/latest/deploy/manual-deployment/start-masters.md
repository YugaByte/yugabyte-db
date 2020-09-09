---
title: Start YB-Master nodes of YugabyteDB cluster
headerTitle: Start YB-Masters
linkTitle: 3. Start YB-Masters
description: Start YB-Master servers of your YugabyteDB cluster
aliases:
  - /deploy/manual-deployment/start-masters
menu:
  latest:
    identifier: deploy-manual-deployment-start-masters
    parent: deploy-manual-deployment
    weight: 613
isTocNested: true
showAsideToc: true
---

{{< note title="Note" >}}

- The number of nodes in a cluster running YB-Masters **must** equal the replication factor.
- The number of comma-separated addresses present in `master_addresses` should also equal the replication factor.
- For running a single cluster across multiple data centers or 2 clusters in 2 data centers, refer to the [Multi-DC deployments](../../../deploy/multi-dc/) section.

{{< /note >}}

This section covers deployment for a single region or data center in a multi-zone/multi-rack configuration. Note that single zone configuration is a special case of multi-zone where all placement-related flags are set to the same value across every node.

## Example scenario

- Create a six-node cluster with replication factor of `3`.
      - YB-Master server should run on only three nodes, but as noted in the next section, the YB-TServer server should run on all six nodes.
      - Assume the three YB-Master private IP addresses are `172.151.17.130`, `172.151.17.220` and `172.151.17.140`.
      - Cloud will be `aws`, region will be `us-west`, and the three AZs will be `us-west-2a`, `us-west-2b`, and `us-west-2c`. Two nodes will be placed in each AZ in such a way that one replica for each tablet (aka shard) gets placed in any one node for each AZ. 
- Multiple data drives mounted on `/home/centos/disk1`, `/home/centos/disk2`.

## Run YB-Master servers with command line flags

Run the `yb-master` server on each of the three nodes as shown below. Note how multiple directories can be provided to the [`--fs_data_dirs`](../../../reference/configuration/yb-master/#fs-data-dirs) flag. Replace the [`--rpc_bind_addresses`](../../../reference/configuration/yb-master/#rpc-bind-addresses) value with the private IP address of the host as well as the set the `placement_cloud`,`placement_region` and `placement_zone` values appropriately. For single zone deployment, use the same value for the `placement_zone` flag.

```sh
$ ./bin/yb-master \
  --master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 \
  --rpc_bind_addresses 172.151.17.130:7100 \
  --fs_data_dirs "/home/centos/disk1,/home/centos/disk2" \
  --placement_cloud aws \
  --placement_region us-west \
  --placement_zone us-west-2a \
  >& /home/centos/disk1/yb-master.out &
```

For the full list of configuration flag), see the [YB-Master reference](../../../reference/configuration/yb-master/).

## Run YB-Master servers with configuration file

Alternatively, you can also create a `master.conf` file with the following flags and then run `yb-master` with the [`--flagfile`](../../../reference/configuration/yb-master/#flagfile) option as shown below. For each YB-Master server, replace the [`--rpc-bind-addresses`](../../../reference/configuration/yb-master/#rpc-bind-addresses) configuration flag with the private IP address of the YB-Master server.

```sh
--master_addresses=172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100
--rpc_bind_addresses=172.151.17.130:7100
--fs_data_dirs=/home/centos/disk1,/home/centos/disk2
--placement_cloud=aws 
--placement_region=us-west 
--placement_zone=us-west-2a 
```

```sh
$ ./bin/yb-master --flagfile master.conf >& /home/centos/disk1/yb-master.out &
```

## Verify health

Make sure all the three YB-Masters are now working as expected by inspecting the INFO log. The default logs directory is always inside the first directory specified in the [`--fs_data_dirs`](../../../reference/configuration/yb-master/#fs-data-dirs) flag.

```sh
$ cat /home/centos/disk1/yb-data/master/logs/yb-master.INFO
```

You can see that the three YB-Masters were able to discover each other and were also able to elect a Raft leader among themselves (the remaining two act as Raft followers).

For the masters that become followers, you will see the following line in the log.

```
I0912 16:11:07.419591  8030 sys_catalog.cc:332] T 00000000000000000000000000000000 P bc42e1c52ffe4419896a816af48226bc [sys.catalog]: This master's current role is: FOLLOWER
```

For the master that becomes the leader, you will see the following line in the log.

```
I0912 16:11:06.899287 27220 raft_consensus.cc:738] T 00000000000000000000000000000000 P 21171528d28446c8ac0b1a3f489e8e4b [term 2 LEADER]: Becoming Leader. State: Replica: 21171528d28446c8ac0b1a3f489e8e4b, State: 1, Role: LEADER
```

{{< tip title="Tip" >}}

Remember to add the command with which you launched `yb-master` to a cron to restart it if it goes down.

{{< /tip >}}

Now you are ready to start the YB-TServers.
