---
title: Delete a universe using Yugabyte Platform
headerTitle: Delete a universe
linkTitle: Delete a universe
description: Use Yugabyte Platform to delete a universe.
block_indexing: true
menu:
  v2.1:
    identifier: delete-universe
    parent: enterprise-edition
    weight: 760
isTocNested: true
showAsideToc: true
---

To delete a universe, in the YugabyteDB Admin Console, select **Delete Universe** from the **More** drop-down on the **Universe Detail** page. For public clouds, such as AWS and Google Cloud Platform, the underlying compute instances are terminated after the database has been installed from those nodes. For on-premises data centers, the underlying compute instances are no longer marked as `In Use` which then opens those instances up to be used in new universes.

![Delete Universe Dropdown](/images/ee/delete-univ-1.png)

![Delete Universe Confirmation](/images/ee/delete-univ-2.png)
