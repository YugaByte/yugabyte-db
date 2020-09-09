---
title: Recover failed tserver/master
linkTitle: Recover tserver/master
description: Recover tserver/master
block_indexing: true
menu:
  v2.1:
    parent: troubleshoot-cluster
    weight: 828
isTocNested: true
showAsideToc: true
---

Suppose you have a cluster running and now a tserver or master process/node has failed.
Here are the steps to take to get the cluster back to shape:

{{< note title="Note" >}}
In this scenario you have a N-node setup, with replication factor (RF)=3.
{{< /note >}}


## Monitoring processes
It's a good idea to have a cron/systemd setup to ensure that the yb-master/yb-tserver process is restarted if it is not running. 
This handles transient failures (such as a node rebooting or process crash due to a bug/some unexpected behavior).

## Node failure
If a node is down, the system will automatically heal and continue to function with the remaining N-1 nodes. 
If the down node doesn't come back soon enough, and N-1 >= 3, 
then tablets which are now under-replicated will be re-replicated automatically to get back to RF=3 on the remaining N-1 nodes.

## Permanent failures
If the node failure is a permanent failure, for the yb-tserver, simply starting another yb-tserver process on the new node is sufficient. 
It’ll join the cluster and load-balancer will automatically take the new yb-tserver into consideration and start rebalancing tablets to it.

## Master failure
If a new yb-master needs to be started to replace a failed master, the master quorum needs to be updated. 
Suppose, the original yb-masters were n1, n2, n3. And n3 needs to be replaced with a new yb-master n4. Then you'll use the `yb-admin` sub-command `change_master_config`:
```
./bin/yb-admin -master_addresses n1:7100,n2:7100 change_master_config REMOVE_SERVER n3 7100
./bin/yb-admin -master_addresses n1:7100,n2:7100 change_master_config ADD_SERVER n4 7100
```
 
The TServers' in-memory state will automatically learn of the new master after the `ADD_SERVER` step, and will not need a restart. 
But you should also update the config file of all yb-tserver processes which specifies the master addresses to reflect the new quorum (n1, n2, n4). 

This is to handle the case if the `yb-tserver` restarts at some point in the future.

## Planned cluster changes
You might also be interested in how to perform planned cluster changes (such as moving the entire cluster to a brand new set of nodes -- say move from machines of type A to type B). 

For that see [changing cluster config](../../../manage/change-cluster-config).

