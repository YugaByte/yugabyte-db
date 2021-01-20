---
title: Delete a universe
headerTitle: Delete a universe
linkTitle: Delete a universe
description: Use Yugabyte Platform to delete a universe.
aliases:
  - /latest/manage/enterprise-edition/delete-universe/
menu:
  latest:
    identifier: delete-universe
    parent: manage-deployments
    weight: 85
isTocNested: true
showAsideToc: true
---

To delete a universe, in the Yugabyte Platform console, select **Delete Universe** from the **More** drop-down list on the **Universe Detail** page.

- For public clouds, such as Amazon Web Services (AWS) and Google Cloud Platform (GCP), the underlying compute instances are terminated after the database has been installed from those nodes.
- For on-premises data centers, the underlying compute instances are no longer marked as `In Use` which then opens those instances up to be reused for new universes.

![Delete Universe Dropdown](/images/ee/delete-univ-1.png)

![Delete Universe Confirmation](/images/ee/delete-univ-2.png)
