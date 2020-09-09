---
title: System statistics
linkTitle: System statistics
headerTitle: Check system statistics
description: How to check system statistics on your YugabyteDB cluster
block_indexing: true
menu:
  v2.1:
    parent: troubleshoot-nodes
    weight: 846
isTocNested: true
showAsideToc: true
---

## Host resource usage

To check the CPU, memory, and disk usage on a Linux machine, you can run the following command.

```sh
$ sudo echo -n "CPUs: ";cat /proc/cpuinfo | grep processor | wc -l; echo -n "Mem: ";free -h | grep Mem | tr -s " " | cut -d" " -f 2; echo -n "Disk: "; df -h / | grep -v Filesystem; 
```

```
CPUs: 72
Mem: 251G
Disk: /dev/sda2       160G   13G  148G   8% /
10.1.12.104
CPUs: 88
Mem: 251G
Disk: /dev/sda2       208G   22G  187G  11% /
10.1.12.105
CPUs: 88
Mem: 251G
Disk: /dev/sda2       208G  5.1G  203G   3% /
```

Generally, common tools like `top` or `iostat` may be useful.

### Auditd

If `top` reports high CPU usage for the `auditd` process, it may have some rules auditing some system calls frequently used YugabyteDB which can significantly affect performance. You can try temporarily disabling `audit` by running (on each YugabyteDB node).

```sh
$ auditctl -e 0
```

and check if this improves performance.

To re-enable it afterwards, run:

```sh
$ auditctl -e 1
```

## YugabyteDB processes state

YugabyteDB provides web endpoints where the current state of each process is aggregated. This includes logs, flags as well as memory, disk, and network usage metrics. Additionally, it provides dedicated metrics endpoints for YCQL and, respectively, Redis requests.

| Description | URL |
|-------------|-----|
| Master Web Page | `<node-ip>:7000` |
| TServer Web Page | `<node-ip>:9000` |
| Redis Metrics | `<node-ip>:11000/metrics` |
| YCQL Metrics | `<node-ip>:12000/metrics` |

{{< note title="Note" >}}

When running `yb-ctl` locally with default values, three local IP addresses will be created: `127.0.0.1`, `127.0.0.2`, and `127.0.0.3`, one for each YugabyteDB node.

{{< /note >}}
