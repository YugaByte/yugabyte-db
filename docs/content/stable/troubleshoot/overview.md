---
title: Troubleshooting
linkTitle: Troubleshooting
headerTitle: Troubleshooting overview
description: Troubleshooting overview
aliases:
  - /troubleshoot/overview/
block_indexing: true
menu:
  stable:
    identifier: troubleshooting-overview
    parent: troubleshoot
    weight: 702
isTocNested: true
showAsideToc: true
---

## 1. Verify that YugabyteDB is running

First, ensure that the expected YugabyteDB processes are running on the current node.
At a minimum, the tserver process needs to be running to be able to connect to this node with a YCQL client or application.

Additionally, depending on the setup, you might expect a master process to also be running on this node.
Follow the instructions on the [check processes](../nodes/check-processes/) page.

## 2. Check cluster-level issues

Next, check the list of [cluster issues](../cluster) and the respective fixes for each of them.

## 3. Check logs

Inspect the YugabyteDB logs for more details on your issue. For more details on where to find and how to understand the YugabyteDB log files, see [Inspect logs](../nodes/check-logs).

## 4. File an issue

If you could not find a solution to your problem in these docs, please file a [GitHub issue](https://github.com/yugabyte/yugabyte-db/issues) describing your specific problem.
