---
title: Node status and actions in Yugabyte Platform
headerTitle: Node status and actions
linkTitle: Node status and actions
description: Use Yugabyte Platform to view Node status and perform actions.
block_indexing: true
menu:
  v2.1:
    identifier: node-status-actions
    parent: enterprise-edition
    weight: 745
isTocNested: true
showAsideToc: true
---

Each node in a universe has a **Status** column indicating its current logical state based on the Yugabyte Platform layer. This section first describes what each of them means and gives information on when and how these can be modified by the user. The **Nodes** tab on the universe page shows them. When you create a universe, the **Nodes** tab on the universe page provides the physical (such as ip, cloud) and the logical state information (master, status, etc.) about that node. Note that the `STATUS` for each node is **Live**. This is the steady state value for a normally functioning node.

![Node Actions](/images/ee/node-actions-live.png)

## Node status

The current list of status columns and some related information.

Status  |  Description
--------|--------------
To Be Added | The backing instance for this node is being created on the IaaS.
Live | Steady state default status of a healthy node. All the server processes are running on that node.
Stopped | Server processes (`yb-tserver` and `yb-master`, if applicable) on that node have been stopped.
Removed | The node does not have any more data and the server processes have been stopped. It is not participating in the universe.
Decommissioned | The underlying instance has been released to the IaaS. This can happen only after a node is removed or processes stopped.
Provisioned | During universe creation, this indicates the instance is being setup with the required OS packages (for example, `ntp` and `chronyd`).
Software Installed | During universe creation, this indicates YugabyteDB software is being deployed on this node.
Upgrade Software | The software version is being updated on this node and the master/tserver processes will be restarted.
Upgrade Flags | A server (`master` or `tserver`) process configuration file is being updated and the corresponding server will be restarted.
Unreachable | Yugabyte Platform is not able to get information from the `node_exporter` process on that node.
Destroyed | Very transient, while the universe (or cluster) is being deleted.

## Node actions

For some of node statuses, there are some actions that are allowed to help make changes to that nodes' backing IaaS instance. They provide a way to handle errors per node and seamlessly take care of data migration in case of instance failures and load balancing in case of node addition.

Status  |  Allowed Actions | Description
--------|--------------|----------------
To Be Added | Delete | This action can be taken if a universe create fails and node is stuck in 'To Be Added'. Once this action is performed, the node (and its underlying instance) will be removed from this universe.
Live |  Stop Processes, Remove Node | The server processes on the node will be stopped, node status becomes 'Stopped'. A Live node can be also marked as 'Removed', which moves the data out of it along with stopping the server processes running on that node. Note that the backing instance is still under the control of the universe. This removes the MASTER/TSERVER setting of the node on the UI.
Stopped | Start Processes, Release Instance | The server processes that are stopped on that node can be restarted using the 'Start Processes' pulldown option. The other option for a 'Stopped' node is to release the backing instance to IaaS and that will stop tracking the ip of this node in the universe.
Removed | Add Node, Release Instance | A removed node can be added back - this restarts the processes on that node and move data onto it from other nodes, and marks it 'Live'. The other option is to release the backing instance to IaaS, and this will stop tracking the ip of this node in the universe and the node will be marked 'Decommissioned'.
Decommissioned | Add Node | A new instance will be used or spawned to replace the released instance, server processes restarted and data load balanced onto this node. It will become 'Live' after this operation.

Rest of the status types do not have any user actions, as they are mostly transient and will end up in one of the above statuses.

{{< note title="Note" >}}

**Add Node** just recreates a new backing instance for an existing node in the universe. To add a completely new node (as in, increase the number of nodes in the universe), one can use the **Edit Universe** option to expand the universe.

{{< /note >}}

The rest of this page describes how to modify the state of each node in a universe. The UI provides different actions that can be taken against each node under the **ACTION** column drop down.

There are two broad set of use cases:

## Long duration operations needing removal of underlying instance

Following are the steps to ensure that an underlying instance for a given node can be replaced with a new instance without any data loss.

### Removal of an instance that is dying

Let's say, for example, that a VM or machine in the universe is hitting end of life or having unrecoverable hardware or other system (OS, disk, etc.) problems. The machine crashes for good, and so there are no processes running on it. This will be detected by the UI and shown as an `Unreachable` node. Note that RAFT will ensure other leaders will get elected for the underlying data shards. But the universe is in an partial under replicated scenario and will not be able to tolerate many more failures. So a quick remedy is needed.

![Unreachable Node Actions](/images/ee/node-actions-unreachable.png)

So, for the `yb-14-node-actions-n3` node, you can use the dropdown **Actions** at the end and choose the **Remove Node** option. This will bring the node to the `Removed` status and no further data can be accessed on this node (as server processes are shut down).

![Remove Node Actions](/images/ee/node-actions-removed.png)

Note that there is no `MASTER/TSERVER` shown for that node. If that node was the `Master Leader`, then the RAFT level election will move it to another node quickly. Similar leader elections happen for tablets for which this node was the the leader tablet server.

### Release instance

Since you know that the instance is dead, you can go ahead and release the ip as well using the 'Release Instance' dropdown option at the end of the Removed node. It will show up as a `Decommissioned` node.

![Release Node Actions](/images/ee/node-actions-released.png)

### Add node

The node can brought back to life on a new backing instance using the **Add Node** option from the dropdown for the `Decomissioned` node. For IaaS, like AWS and GCP, the Yugabyte Platform application will spawn with the existing instance type in the correct/existing region and zone of that node. After the end of this operation, the node will have `yb-master` and `yb-tserver` processes running along with some data that is load balanced onto this node and status will be marked `Live`. Note that the node name is reused and is part of the healthy cluster now.

![Add Node Actions](/images/ee/node-actions-add-node.png)

{{< note title="Note" >}}

Do not `REMOVE` more than (RF - 1)/2 nodes at any given time. For example, on a RF=3 cluster with three server nodes, there can only be one removed node. This is needed for [RAFT](https://raft.github.io/) consensus algorithm. We will be adding safeguards against this soon.

{{< /note >}}

{{< note title="Note" >}}

`REMOVE NODE` will not work for the case where the number of nodes is equal to the RF of the cluster. Since there is no other nodes to move the data via the load balancer.

{{< /note >}}

## Quick operations, on an existing instance

The second scenario is for more of a 'quick' planned change that can be performed on a node. For example, the DevOps wants to mount a new disk on the node or just install and run a new security daemon. In that case, the instance is still in use and stopping any running YugabyteDB process might be needed. Then the user can select the **Stop Processes** option, perform the system task, and then select **Start Processes** for that node.

The following two steps helps stop the server processes on the node and restart it back up. There is no data moved out of the node proactively, but the data shard/tablet leaders could change based on RAFT requirements.

### Stop processes

Let's say `yb-14-node-actions-n4` is the node that needs the intervention, then you would pick the **Stop Processes** option.

![Stop Node Actions](/images/ee/node-actions-stop.png)

Once the `yb-tserver` (and `yb-master`, if applicable) are stopped, the node status is updated and the instance is ready for the planned system changes.

![Stop Node Actions](/images/ee/node-actions-stopped.png)

{{< note title="Note" >}}

Do not STOP more than (RF - 1)/2 processes at any given time. For example, on an RF=3 cluster with three nodes, there can only be one node with stopped processes to allow majority of nodes to perform consensus operations.

{{< /note >}}

### Start processes

After the work is complete, the processes can be restarted using the same dropdown for that node.

![Start Node Actions](/images/ee/node-actions-start.png)

The node will go back to the `Live` state once the processes are up and running.

In the worst case scenario, when the system runs into some unrecoverable errors at this stage, there is a **Release Instance** option for the stopped node, which will help remove the backing instance as well, as described above.

## Node action task summary

As a summary of all the actions that were run on this universe, one can check the **Tasks** tab to see the remove/add and stop/start tasks that were run on that universe.

![Tasks Node Actions](/images/ee/node-actions-tasks.png)

{{< note title="Note" >}}

In any universe, one cannot have more than (RF - 1)/2 `Master` nodes in `Stop` or `Remove` status at the same time.

{{< /note >}}

## Interaction with other operations

If there is a node in any of the in-transit states of `Stopped`, `Removed` or `Decommissioned` in the universe, you disallow [edit operations](../edit-universe/) and [rolling upgrade operations](../upgrade-universe/). These operations are allowed once such a node comes out of that in-transit state.
