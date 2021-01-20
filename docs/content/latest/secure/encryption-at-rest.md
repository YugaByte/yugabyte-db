---
title: Enable encryption at rest in YugabyteDB clusters
headerTitle: Encryption at rest
linkTitle: Encryption at rest
description: Enable encryption at rest in a YugabyteDB cluster with a user-generated key.
headcontent: Enable encryption at rest with a user-generated key
image: /images/section_icons/secure/prepare-nodes.png
aliases:
  - /secure/encryption-at-rest
menu:
  latest:
    identifier: encryption-at-rest
    parent: secure
    weight: 735
isTocNested: true
showAsideToc: true
---

This page describes how to enable and disable encryption at rest in a YugabyteDB cluster with a user-generated key.

## Enabling encryption

### Step 1. Create encryption key

First, you will generate the universe key data. This data can have length 32, 40, or 48. Larger keys are slightly more secure with slightly worse performance. Run the following on your local filesystem.

```sh
$ openssl rand -out universe_key [ 32 | 40 | 48 ]

```

### Step 2. Copy key to master nodes

In this example, assume a 3 node RF=3 cluster with addresses ip1, ip2, ip3.
Copy the universe key onto each master filesystem, in the same location on every node.

```sh
$ for ip in ip1 ip2 ip3
  do
    scp -i <ssh_key> universe_key ip:/mnt/d0/yb-data/master
  done
```

{{< note title="Note" >}}

The key can live in any subdirectory of the master directory, as long as it lives in the same place on each node. In addition, the data directory may vary depending on how the cluster is created.

{{< /note >}}

### Step 3. Enable cluster-wide encryption

Use yb-admin to tell the cluster about the new universe key.

```sh
$ yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 rotate_universe_key
/mnt/d0/yb-data/master/universe_key
```

{{< note title="Note" >}}
Because data is encrypted in the background as part of flushes to disk and compactions, only new data will be encrypted. Therefore, the call should return quickly.
{{< /note >}}

### Step 4. Verify encryption enabled

To check the encryption status of the cluster, run the following yb-admin command.

```sh
$ yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 is_encryption_enabled
```

```
Encryption status: ENABLED with key id <key_id>
```

## Rotating a new key

### Step 1. Creating a new key

First you create the key to be rotated.

```sh
$ openssl rand -out universe_key_2 [ 32 | 40 | 48 ]

```

{{< note title="Note" >}}
The new key name must be distinct from the previous key name.
{{< /note >}}

### Step 2. Copy new key to master nodes

As with enabling, copy the universe key onto each master filesystem,
in the same location on every node.

```sh
$ for ip in ip1 ip2 ip3
  do
    scp -i <ssh_key> universe_key ip:/mnt/d0/yb-data/master/
  done
```

### Step 3. Rotate key

Use yb-admin to tell the cluster about the new universe key.

```sh
$ yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 rotate_universe_key
/mnt/d0/yb-data/master/universe_key_2
```

### Step 4. Verify new key

Check that the new key is encrypting the cluster.

```sh
$ yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 is_encryption_enabled
```

```
Encryption status: ENABLED with key id <key_id_2>
```

`<key_id_2>` should be different from the previous `<key_id>`.

## Disable encryption

### Step 1. Disable cluster-wide encryption

Use yb-admin to disable encryption.

```sh
$ yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 disable_encryption
```

### Step 2. Verify encryption disabled

Check that encryption is disabled.

```sh
$ yb-admin -master_addresses ip1:7100,ip2:7100,ip3:7100 is_encryption_enabled
```

```
Encryption status: DISABLED
```
