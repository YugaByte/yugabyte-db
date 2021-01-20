---
title: Replace a failed YB-TServer
headerTitle: Replace a failed YB-TServer
linkTitle: Replace a failed YB-TServer
description: Procedure to replace a failed YB-TServer
menu:
  latest:
    identifier: replace-failed-tserver
    parent: troubleshoot-cluster
    weight: 829
isTocNested: true
showAsideToc: true
---

If you have a failed YB-TServer in a YugabyteDB cluster that needs to be replaced, follow these steps:

## Start the new YB-TServer

Install and then start a new YB-TServer, making sure it is in the same placement group as the one you are replacing.

For details on starting YB-TServers and more options, see [Start YB-TServers](../../../../deploy/manual-deployment/start-tservers/).

## Blacklist the failed YB-TServer

To blacklist the failed YB-TServer, run the following command:
 
```sh
~/master/bin/yb-admin -master_addresses $MASTERS change_blacklist ADD $OLD_IP:9100
```

For details, see [change_blacklist](../../../admin/yb-admin#change-blacklist) command in the yb-admin reference page.

## Wait for the rebalance to complete

Wait for the data to drain from the failed YB-TServer and for the data to be loaded into the new one. You can check for the completion of rebalancing by running the following command:

```sh
~/master/bin/yb-admin -master_addresses $MASTERS get_load_move_completion 
```

{{< note title="Note" >}}

Loading and rebalancing will complete if only data from the failed YB-TSserver has someplace to be stored. 
Start the new YB-TServer first, or ensure your remaining YB-TServers have enough capacity and are in the correct placement zones.

{{< /note >}}

For details on using this command, see [`get_load_move_completion`](../../admin/yb-admin.md#get-load-move-completion).

## Kill the failed YB-TServer

When the data move is complete (100%), kill the failed YB-TServer by stopping the `yb-tserver` process, or terminating the VM.

Now, wait for the YB-TServer to be marked as `DEAD` by the YB-Master leader.  
The YB-Master leader will mark the server as `DEAD` after not responding for one minute (based on the `tserver_unresponsive_timeout_ms` (default is `60000`).
 
To verify that the failed YB-TServer is dead, open your web browser to `$MASTER_LEADER_IP:7000/tablet-servers` and check the output.

## Remove the failed YB-TServer from the blacklist 

Now that the replacement YB-TServer is up and running, and loaded with data, remove the address for the failed YB-TServer from the blacklist.

```sh
~/master/bin/yb-admin -master_addresses $MASTERS change_blacklist REMOVE node1:9100
```
