---
title: Recover YB-TServer from crash loop
linkTitle: Recover YB-TServer from crash loop
description: Recover YB-TServer from crash loop
aliases:
  - /troubleshoot/cluster/recover-tserver-failed-tablets/
  - /latest/troubleshoot/cluster/recover-tserver-failed-tablets/
menu:
  latest:
    parent: troubleshoot-cluster
    weight: 830
isTocNested: true
showAsideToc: true
---

When a YB-TServer process or node has failed, YugabyteDB will automatically trigger a remote bootstrap for
 most types of tablet data corruption or failures. 

However, there are still a number of cases where this automatic mechanism might not be able to help.

One major such case is anything that leads to a code path that we've encoded as a crash (eg: `CHECK` or `FATAL`) or is an unknown unknown 
that causes a crash (eg: code bugs leading to `SIGSEGV`). Such crashes are likely repeatable, leading to the server
 being stuck in a crash loop, hence runtime automatic fixes would not be able to kick in, as the server would not get
  to a steady state yet.

{{< note title="Note" >}}
Normally, to fix this, we need to know the tablet(s) that are hitting these problems. We can [look into logs](../../nodes/check-logs) to get the UUID of tablet(s). 

In this scenario, the UUID of the tablet is `FOO` and the `--fs_data_dirs` flag for these is `/mnt/disk1`.
{{< /note >}}

Here are the steps to address this scenario:

1. Stop the yb-tserver process (to prevent new restarts, during operations)
2. Find and remove all the tablet files: 
```bash
find /mnt/disk1 -name '*FOO*' | xargs rm -rf
```
3. You need to repeat the command above for each disk in `--fs_data_dirs`
3. Start back the yb-tserver process

Now the yb-tserver will be able to start and will trigger a remote bootstrap for the tablet that we just deleted.
