---
title: Troubleshoot install and upgrade issues
headerTitle: 
linkTitle: Install and upgrade
description: Troubleshoot issues encountered during installing or upgrading Yugabyte Platform.
menu:
  stable:
    identifier: install-upgrade-yp-issues
    parent: troubleshoot-yp
    weight: 10
isTocNested: true
showAsideToc: true
---

Here are some common issues encountered during installing or upgrading Yugabyte Platform. If you don't find an answer, contact Yugabyte Support.

## SELinux turned on the Yugabyte Platform host

If your host has SELinux turned on, then Docker Engine might not be able to connect with the host. To open the ports using firewall exceptions, run the following command.

```sh
sudo firewall-cmd --zone=trusted --add-interface=docker0
sudo firewall-cmd --zone=public --add-port=80/tcp
sudo firewall-cmd --zone=public --add-port=443/tcp
sudo firewall-cmd --zone=public --add-port=8800/tcp
sudo firewall-cmd --zone=public --add-port=5432/tcp
sudo firewall-cmd --zone=public --add-port=9000/tcp
sudo firewall-cmd --zone=public --add-port=9090/tcp
sudo firewall-cmd --zone=public --add-port=32769/tcp
sudo firewall-cmd --zone=public --add-port=32770/tcp
sudo firewall-cmd --zone=public --add-port=9880/tcp
sudo firewall-cmd --zone=public --add-port=9874-9879/tcp
```


## Create mount paths on the nodes

Create mount paths on the nodes with private IP addresses: `10.1.13.150`, `10.1.13.151`, `10.1.13.152`.

```sh
for IP in 10.1.12.103 10.1.12.104 10.1.12.105; do ssh $IP mkdir -p /mnt/data0; done
```

## SELinux turned on for nodes

Add firewall exceptions on the nodes with private IP addresses: `10.1.13.150`, `10.1.13.151`, `10.1.13.152`.

```sh
for IP in 10.1.12.103 10.1.12.104 10.1.12.105
do
  ssh $IP firewall-cmd --zone=public --add-port=7000/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=7100/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=9000/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=9100/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=11000/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=12000/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=9300/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=9042/tcp;
  ssh $IP firewall-cmd --zone=public --add-port=6379/tcp;
done
```
