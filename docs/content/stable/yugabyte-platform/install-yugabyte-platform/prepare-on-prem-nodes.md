---
title: Prepare nodes (on-prem)
headerTitle: Prepare nodes (on-prem)
linkTitle: Prepare nodes (on-prem)
description: Prepare YugabyteDB nodes for on-premises deployments.
menu:
  stable:
    identifier: prepare-on-prem-nodes
    parent: install-yugabyte-platform
    weight: 79
isTocNested: true
showAsideToc: true
---

For on-premises deployments of YugabyteDB universes, you need to import nodes which can be managed by Yugabyte Platform. This page outlines the steps required to prepare these YugabyteDB nodes for on-premises deployments.

{{< note title="Note" >}}

For airgapped, on-premises deployments (without Internet connectivity), additional steps are required as outlined below.

{{< /note >}}

Ensure that the YugabyteDB nodes conform to the requirements outlines in the [deployment checklist](../../../deploy/checklist/). This checklist also gives an idea of [recommended instance types across public clouds](../../../deploy/checklist/#running-on-public-clouds).

Install the prerequisites and verify the system resource limits as described in [system configuration](../../../deploy/manual-deployment/system-config).

## SSH user

Identify or create an SSH user. This user must have `sudo` privileges on each YugabyteDB node.

To add a new SSH user, follow this procedure:

1. Add the `yugabyte` group.

```sh
$ sudo groupadd yugabyte
```

2. Add the SSH user `yugabyte`.

```ssh
$ sudo useradd -m -s /bin/bash -g yugabyte yugabyte
```

3. Add a password for the user `yugabyte`.

```sh
$ sudo passwd yugabyte
```

4. Ensure the `/home/yugabyte` home directory exists for this user. The directory should have been created automatically as a result of the above steps.

5. Verify the `/home/yugabyte` directory is owned by `yugabyte:yugabyte`.

## Access as `sudo`

1. Add the `yugabyte` user to the `sudo` users file (`/etc/sudoers`) using the `visudo` command. 

2. Add the line below to the end of the file and then save your changes:

```sh
yugabyte	ALL=(ALL)	NOPASSWD: ALL
```

## Passwordless access

1. Add the SSH keys to enable passwordless SSH as the `yugabyte` user:

```sh
$ sudo mkdir /home/yugabyte/.ssh
$ sudo chown yugabyte:yugabyte /home/yugabyte/.ssh
$ sudo chmod 700 /home/yugabyte/.ssh
$ sudo touch /home/yugabyte/.ssh/authorized_keys
$ sudo chmod 600 /home/yugabyte/.ssh/authorized_keys
$ sudo chown yugabyte:yugabyte /home/yugabyte/.ssh/authorized_keys
```

2. Add the public key of the on-premises provider to authorized keys for this user. This is a public key derived from the provider private key created when installing the Yugabyte Platform:

```sh
$ ssh-keygen -y -f <private-key-file>.pem
```

3.Verify that you can `ssh` into this node (from your local machine, if node has a public address).

```ssh
$ ssh -i your_private_key.pem yugabyte@node_ip
```
