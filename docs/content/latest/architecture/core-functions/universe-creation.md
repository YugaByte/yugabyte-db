---
title: Universe creation
headerTitle: Universe creation
linkTitle: Universe creation
description: Learn how YugabyteDB creates a universe with YB-TServer and YB-Master nodes.
aliases:
  - /architecture/core-functions/universe-creation/
menu:
  latest:
    identifier: universe-creation
    parent: core-functions
    weight: 1182
isTocNested: false
showAsideToc: true
---

## Step 1. Start YB-Masters

When creating a YugabyteDB universe, the first step is to bring up sufficient YB-Masters (as many as
the replication factor) with each being told about the others. These YB-Masters initialize themselves with a unique
UUID, learn about each other and perform a leader election. At the end of this step, one of the masters establishes itself as the leader.

## Step 2. Start YB-TServers

The next step is to start as many YB-TServers as there are nodes, with the master addresses being
passed to them on startup. They start heart-beating to the masters, communicating the fact that they
are alive. The heartbeats also communicate the tablets the YB-TServers are currently hosting and
their load, but no tablets would exist in the system yet.

## An example

Let us illustrate this with our usual example of creating a 4-node YugabyteDB universe with a
replication factor of 3. First the three masters are started in create mode. This is done explicitly
to prevent accidental errors in creating a universe while it is already running.

![create_universe_masters](/images/architecture/create_universe_masters.png)

The next step, the masters learn about each other and elect one leader.

![create_universe_master_election](/images/architecture/create_universe_master_election.png)

The YB-TServers are then started, and they all heartbeat to the YB-Master.

![create_universe_tserver_heartbeat](/images/architecture/create_universe_tserver_heartbeat.png)
