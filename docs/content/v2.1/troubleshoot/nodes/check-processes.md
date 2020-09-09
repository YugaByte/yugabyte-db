---
title: Check processes
linkTitle: Check processes
headerTitle: Check YugabyteDB processes
description: How to check if your YugabyteDB processes are running
block_indexing: true
menu:
  v2.1:
    parent: troubleshoot-nodes
    weight: 842
isTocNested: true
showAsideToc: true
---

## 1. Are the YugabyteDB processes running?

Connect to the local node where YugabyteDB is running. 

On the local setup, this is your local machine (or a Docker instance running on your local machine). On a multi-node cluster, you may need to `ssh` into the machines where the YugabyteDB nodes are running.

```sh
$ ps aux | grep yb-tserver
```

If you are expecting a master process on this node, you can also do the following.

```sh
$ ps aux | grep yb-master
```

If the processes are not running you can start them with:

- `yb-ctl` when using a local cluster.

- `bin/yb-tserver` and `bin/yb-master` servers when using a multi-node cluster.

Once the processes are running, if they are not accessible from your client machine this may be a network issue (see below).

## 2. Are the yb-master and yb-tserver endpoints accessible?

Generally, the endpoints are:

|      Description |                       URL |
|------------------|---------------------------|
| Master Web Page  | `<node-ip>:7000`          |
| TServer Web Page | `<node-ip>:9000`          |
| Redis Metrics    | `<node-ip>:11000/metrics` |
| YCQL Metrics      | `<node-ip>:12000/metrics` |
| Redis Server     | `<node-ip>:6379`          |
| YCQL Server       | `<node-ip>:9042`          |

However, in some setups these endpoints may not be accessible, depending on the configuration on your physical machines or on your cloud-provider account.

### IP not accessible

- Private versus Public IP: Consider setting up a VPN or using the node’s public IP (for example, get it from the machine status on your cloud-provider account).

### Ports closed

- Cloud Account Configuration: Open the relevant ports (see below), for TCP traffic on your cloud-provider account (for example, security group rules).

- SELinux turned on: If your host has SELinux turned on, run the following commands to open the ports using firewall exceptions.

```sh
sudo firewall-cmd --zone=public --add-port=7000/tcp;
sudo firewall-cmd --zone=public --add-port=7100/tcp;
sudo firewall-cmd --zone=public --add-port=9000/tcp;
sudo firewall-cmd --zone=public --add-port=9100/tcp;
sudo firewall-cmd --zone=public --add-port=11000/tcp;
sudo firewall-cmd --zone=public --add-port=12000/tcp;
sudo firewall-cmd --zone=public --add-port=9300/tcp;
sudo firewall-cmd --zone=public --add-port=9042/tcp;
sudo firewall-cmd --zone=public --add-port=6379/tcp;
```
