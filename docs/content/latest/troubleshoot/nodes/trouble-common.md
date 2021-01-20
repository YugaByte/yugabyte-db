---
title: Common error messages
linkTitle: Common error messages
headerTitle: Understanding common error messages
description: How to understand and recover from common error messages
menu:
  latest:
    parent: troubleshoot
    weight: 800
isTocNested: true
showAsideToc: true
---

## Skipping add replicas

When a new node has joined the cluster or an existing node has been removed, you may see errors messages like below:

```
W1001 10:23:00.969424 22338 cluster_balance.cc:232] Skipping add replicas for 21d0a966e9c048978e35fad3cee31698: 
Operation failed. Try again. Cannot add replicas. Currently have a total overreplication of 1, when max allowed is 1
```

This message is harmless and can be ignored. It means that the maximum number of concurrent tablets being remote bootstrapped across the
 cluster by the YB-Master load balancer has reached its limit. 
 This limit is configured in `--load_balancer_max_concurrent_tablet_remote_bootstraps` in 
 [yb-master config](../../../reference/configuration/yb-master#load-balancer-max-concurrent-tablet-remote-bootstraps).

## SST files limit exceeded

When you encounter the error below:
```
Service unavailable (yb/tserver/tablet_service.cc:257): SST files limit exceeded 58 against (24, 48), score: 0.35422774182913203: 3.854s (tablet server delay 3.854s)
```

This message is emitted when the number of SST files has exceeded its limit. Usually, the client is running a high INSERT/UPDATE/DELETE workload 
and compactions are falling behind. 

To determine why this error is happening, you can check the disk bandwidth, network bandwidth, and find out if enough CPU is available in the server. 
The limits are controlled by the following YB-TServer configuration flags: `--sst_files_hard_limit=48` and `--sst_files_soft_limit=24`.

## Catalog Version Mismatch: A DDL occurred while processing this query. Try Again.

When executing queries in the YSQL layer, the query may fail with the following error:

```
org.postgresql.util.PSQLException: ERROR: Catalog Version Mismatch: A DDL occurred while processing this query. Try Again
```

A DML query in YSQL may touch multiple servers, and each server has a Catalog Version which is used to track schema changes.
When a DDL statement runs in the middle of the DML query, the Catalog Version is changed and the query has a mismatch, causing it to fail.

In these cases, the database aborts the query and returns a `40001` PostgreSQL error code. Errors with this code can be safely
retried from the client side. 
