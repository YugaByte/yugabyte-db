---
title: Disk failure
linkTitle: Disk failure
headerTitle: Recover failing disk
description: Learn how to recover failing disk
block_indexing: true
menu:
  v2.1:
    parent: troubleshoot-nodes
    weight: 849
isTocNested: true
showAsideToc: true
---

YugabyteDB can be configured to use multiple storage disks by setting the [`--fs_data_dirs`](../../../reference/configuration/yb-tserver) flag.
This introduces the possibility of disk failure and recovery issues.

## Cluster replication recovery

The `yb-tserver` service automatically detects disk failures and attempts to spread the data from the failed disk to other healthy nodes in the cluster. In a single-zone setup with a replication factor (RF) of `3`: if you started with four nodes or more, then there would be at least three nodes left after one failed. In this case, re-replication is automatically started if a YB-TServer or disk is down for 10 minutes.

In a multi-zone setup with a replication factor (RF) of `3`: YugabyteDB will try to keep one copy of data per zone. In this case, for automatic re-replication of data, a zone needs to have at least two YB-TServers so that if one fails,
its data can be re-replicated to the other. Thus, this would mean at least a six-node cluster.

## Failed disk replacement

The steps to replace a failed disk are:

1. Stop the YB-TServer node.
2. Replace the disks that have failed.
3. Restart the `yb-tserver` service.

On restart, the YB-TServer will see the new empty disk and start replicating tablets from other nodes.
