---
title: Replace a failed YB-Master
headerTitle: Replace a failed YB-Master
linkTitle: Replace a failed YB-Master
description: Steps to replace a failed YB-Master in a YugabyteDB cluster.
aliases:
  - /troubleshoot/cluster/replace-master/
  - /latest/troubleshoot/cluster/replace-master/
menu:
  latest:
    identifier: replace-failed-master
    parent: troubleshoot-cluster
    weight: 831
isTocNested: true
showAsideToc: true
---

To replace a failed YB-Master server in a YugabyteDB cluster, follow these steps:

For the steps below, the examples use the following scenario:

- The cluster includes three `yb-master` servers: `M1`, `M2`, `M3`.
- YB-Master server `M1` failed and needs to be replaced.
- A new YB-Master server (`M4`) will replace `M1`.
- The default master RPC port is `7100`

{{< note title="Note" >}}

If the master to be replaced is already dead (for example, the VM was terminated), perform the `REMOVE` step first, and then the `ADD` step.

{{< /note >}}


## Start the replacement YB-Master server in standby mode. 

To start `yb-master` in standby mode, set the `--master_addresses` flag to an empty string (`""`). When the
 `--master_addresses` is `""`, this master server starts without joining any existing master quorum. The node will be added to the master quorum in a later step.

```sh
./bin/yb-master --master_addresses="" --fs_data_dirs=<your data directories> [any other flags you would normally pass
 to this master process]
```

## Add the replacement YB-Master server into the existing cluster.

To add the new YB-Master server, run the [`yb-admin change_master_config ADD_SERVER`](../../admin/cli/yb-admin/#change-master-config) command.

```sh
./bin/yb-admin -master_addresses M1:7100,M2:7100,M3:7100 change_master_config ADD_SERVER M4 7100
```

## Remove the failed YB-Master server from the cluster.

To remove the failed YB-Master server from the cluster, use the [`yb-admin change_master_config REMOVE_SERVER`](../../admin/yb-admin/#change-master-config`) command.

```sh
./yb-admin -master_addresses M1:7100,M2:7100,M3:7100,M4:7100 change_master_config REMOVE_SERVER M1 7100
```
{{< note title="Note" >}}

Make sure to specify all YB-Master addresses, including M4, to make sure that if M4 becomes the leader, then yb-admin can find it.

{{< /note >}}
## Validate cluster

Validate that your set of masters is now `M2`, `M3` and `M4` using:
```bash
yb-admin -master_addresses M2:7100,M3:7100,M4:7100 list_all_masters
```

Until [#1542](https://github.com/yugabyte/yugabyte-db/issues/1542) is implemented, the TS will by default only know of 
whatever masters are encoded in the `--tserver_master_addrs` flag that they are started with. 

If any one of those masters is still part of the active quorum, then they will propagate the new master quorum over via heartbeats. 
If, however, none of the current masters are present in the TS flag, then the TS will not be able to join the cluster! 

So it is important to make sure to update `--tserver_master_addrs` on every TS to the new set of master addresses, `M2:7100,M3:7100,M4:7100`!
