---
title: Network security
headerTitle: Network security
linkTitle: Network security
description: Network security
menu:
  stable:
    parent: security
    identifier: network-security
    weight: 30
isTocNested: true
showAsideToc: true
---
To ensure that Yugabyte Platform YugabyteDB runs in a trusted network environment you can restrict machine and port access. Here are some steps to ensure that:

- Servers running YugabyteDB services are directly accessible only by the Yugabyte Platform, servers running the application, and database administrators.
- Only Yugabyte Platform and servers running applications can connect to YugabyteDB services on the RPC ports. Access to the YugabyteDB ports should be denied to everybody else.

Check the list of [default ports](../../reference/configuration/default-ports) that need to be opened on the YugabyteDB servers for the Yugabyte Platform and other applications to connect.
