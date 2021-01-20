---
title: Use Yugabyte Platform to add a node
headerTitle: Add a node
linkTitle: Add a node
description: Use Yugabyte Platform to add a node.
aliases:
  - /latest/manage/enterprise-edition/create-universe-multi-region
menu:
  latest:
    identifier: add-nodes
    parent: manage-deployments
    weight: 40
isTocNested: true
showAsideToc: true
---

The node can brought back to life on a new backing instance using the **Add Node** option from the **Actions** drop-down list for the `Decommissioned` node. For IaaS, like AWS and GCP, Yugabyte Platform will spawn with the existing node instance type in the correct/existing region and zone of that node. After the end of the operation, the node will have `yb-master` and `yb-tserver` processes running along with some data that is load balanced onto this node and status will be marked as `Live`. Note that the node name is reused and is part of the healthy cluster now.

![Add Node Actions](/images/ee/node-actions-add-node.png)
