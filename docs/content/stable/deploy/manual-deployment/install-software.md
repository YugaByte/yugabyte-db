---
title: Install YugabyteDB software
headerTitle: Install software
linkTitle: 2. Install software
description: Download and install YugabyteDB software to each node
aliases:
  - /deploy/manual-deployment/install-software
block_indexing: true
menu:
  stable:
    identifier: deploy-manual-deployment-install-software
    parent: deploy-manual-deployment
    weight: 612
isTocNested: true
showAsideToc: true
---

Install YugabyteDB on each of the nodes using the steps shown below.

## Download

Download the YugabyteDB binary package as described in the [Quick Start section](../../../quick-start/install/).

Copy the YugabyteDB package into each instance and then run the following commands.

```sh
$ tar xvfz yugabyte-<version>-<os>.tar.gz && cd yugabyte-<version>/
```

## Configure

- Run the **post_install.sh** script to make some final updates to the installed software.

```sh
$ ./bin/post_install.sh
```
