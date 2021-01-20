---
title: Start and stop processes
headerTitle: Start and stop processes
linkTitle: Start and stop processes
description: Use Yugabyte Platform to start and stop processes.
menu:
  latest:
    identifier: start-stop-processes
    parent: manage-deployments
    weight: 10
isTocNested: true
showAsideToc: true
---

## Start a process

After the work is complete, the processes can be restarted using the same drop-down list for that node.

![Start Node Actions](/images/ee/node-actions-start.png)

The node will return to the `Live` state once the processes are up and running.

In the worst case scenario, when the system runs into some unrecoverable errors at this stage, there is a **Release Instance** option for the stopped node, which will help remove the backing instance as well. For details, see [Remove a node](../remove-nodes/).

## Stop a process

Let's say `yb-14-node-actions-n4` is the node that needs the intervention, then you would pick the **Stop Processes** option.

![Stop Node Actions](/images/ee/node-actions-stop.png)

Once the `yb-tserver` (and `yb-master`, if applicable) are stopped, the node status is updated and the instance is ready for the planned system changes.

![Stop Node Actions](/images/ee/node-actions-stopped.png)

{{< note title="Important" >}}

Do not stop more than (RF - 1)/2 processes at any given time. For example, on an RF=3 cluster with three nodes, there can only be one node with stopped processes to allow the majority of the nodes to perform Raft consensus operations.

{{< /note >}}
