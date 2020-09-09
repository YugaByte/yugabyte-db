---
title: Edit a universe using Yugabyte Platform
headerTitle: Edit a universe
linkTitle: Edit a universe
description: Use Yugabyte Platform to edit a universe.
aliases:
  - /latest/manage/enterprise-edition/edit-universe/
menu:
  latest:
    identifier: edit-universe
    parent: manage-yugabyte-platform
    weight: 730
isTocNested: true
showAsideToc: true
---

Expanding the universe to add more nodes as well as shrinking the universe to lesser number of nodes is as simple as clicking **Edit** on the **Universe** page and then providing the new user intent for the universe. The new user intent can even be for an entirely new configuration of nodes powered by a different instance type. The YugabyteDB Admin Console will orchestrate this change through the YB-Masters powering this universe. These YB-Masters ensure that the new nodes start hosting the tablet leaders for a set of tablets in such a way that the tablet leader count remains evenly balanced across all the available nodes. This background data replication is undertaken in a throttled manner so that the foreground applications are never impacted.

![Edit universe](/images/ee/edit-univ.png)
