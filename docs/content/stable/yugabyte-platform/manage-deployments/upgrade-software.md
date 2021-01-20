---
title: Upgrade the YugabyteDB software
headerTitle: Upgrade the YugabyteDB software
linkTitle: Upgrade the YugabyteDB software
description: Use Yugabyte Platform to upgrade the YugabyteDB software.
menu:
  stable:
    identifier: upgrade-software
    parent: manage-deployments
    weight: 80
isTocNested: true
showAsideToc: true
---

The YugabyteDB release that is powering a universe can be upgraded to get the new features and fixes in a release.

A rolling upgrade can be performed on a live universe deployment by following these steps:

1. Go to the **Universe Detail** page.
2. From the **More** drop-down list, select **Upgrade Software**.
3. In the confirmation dialog, select the new YugabyteDB version from the drop-down list. The Yugabyte Platform console will upgrade the universe in a rolling manner.

![Upgrade Universe Dropdown](/images/ee/upgrade-univ-1.png)

![Upgrade Universe Confirmation](/images/ee/upgrade-univ-2.png)
