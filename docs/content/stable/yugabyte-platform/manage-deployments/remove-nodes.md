---
title: Use Yugabyte Platform to remove a node
headerTitle: Remove a node
linkTitle: Remove a node
description: Use Yugabyte Platform to remove an unresponsive node.
aliases:
  - /stable/manage/enterprise-edition/create-universe-multi-region
menu:
  stable:
    identifier: remove-nodes
    parent: manage-deployments
    weight: 50
isTocNested: true
showAsideToc: true
---

## Remove an unresponsive node

If a VM or machine in a universe reaches its end of life and has unrecoverable hardware or other system (OS, disk, etc.) issues, it will be detected and displayed in the Yugabyte Platform console as an `Unreachable` node. The Raft consensus algorithm will ensure that other leaders will get elected for the underlying data shards. But, the universe is in a partially under-replicated scenario and will not be able to tolerate many more failures. So a quick remedy is needed.

![Unreachable Node Actions](/images/ee/node-actions-unreachable.png)

So, for the `yb-14-node-actions-n3` node, you can use the **Actions** drop-down list at the end and choose the **Remove Node** option. This will bring the node to the `Removed` status and no further data can be accessed from this node as server processes are shut down.

![Remove Node Actions](/images/ee/node-actions-removed.png)

Note that there is no `MASTER/TSERVER` shown for that node. If that node was the `Master Leader`, then a Raft election will move it to another node quickly. Similar leader elections happen for tablets for which this node was the the leader tablet server.

## Release a node instance

Because you know that the instance is dead, you can go ahead and release the IP address using the **Actions** drop-down list to select the **Release Instance** option at the end of the Removed node. It will show up as a `Decommissioned` node.

![Release Node Actions](/images/ee/node-actions-released.png)
