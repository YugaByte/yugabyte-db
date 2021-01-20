---
title: Customize ports
headerTitle: Customize ports
linkTitle: Customize ports
description: Customize ports
menu:
  latest:
    parent: security
    identifier: customize-ports
    weight: 15
---

Follow the steps below to customize your YugabyteDB ports for security purposes.
For a list of YugabyteDB default ports, see [Default ports reference](../../../reference/configuration/default-ports).

## Steps to customize ports

1. Go to the **Create universe** page and configure as desired.
2. Select **Override Deployment Ports**.

![Create universe - Override Deployment Ports](/images/yp/security/override-deployment-ports.png)

3. Specify the port each process should use. This can be any value from `1` to `65535` (as long as the specified values don’t conflict with anything else running on nodes to be provisioned). In the case of **Node Exporter Port**, the value is used for both what Prometheus will use to scrape node-level metrics as well as what Node Exporter will be configured on nodes to use. If **Install Node Exporter** is not selected and the user is configuring Node Exporter on nodes out of band of Yugabyte Platform, this value should be the port that Node Exporter is already running on nodes with.

![Override Deployment Ports](/images/yp/security/override-deployment-ports.png)

4. Create the universe.
