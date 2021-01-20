---
title: Enable encryption at rest
headerTitle: Enable encryption at rest
linkTitle: Enable encryption at rest
description: Enable encryption at rest
menu:
  latest:
    parent: security
    identifier: enable-encryption-at-rest
    weight: 28
isTocNested: true
showAsideToc: true
---

## Encryption at rest overview

Data at rest within a YugabyteDB universe should be protected from unauthorized users by encrypting it. Using the Yugabyte Platform console, you can:

 <!-- no toc -->

- [Enable encryption at rest during universe creation](#enable-encryption-at-rest-during-universe-creation)
- [Enable encryption at rest on an existing universe](#enable-encryption-at-rest-on-an-existing-universe)
- [Back up and restore data from an encrypted at rest universe](#back-up-and-restore-data-from-an-encrypted-at-rest-universe)
- [Rotate the universe keys used for encryption at rest](#rotate-the-universe-keys-used-for-encryption-at-rest)
- [Disable encryption at rest](#disable-encryption-at-rest)

Two types of keys are used to encrypt data in YugabyteDB:

- Universe key: The top-level symmetric key used to encrypt other keys and are common to the universe.
- Data key: The symmetric key used to encrypt data. There is one data key generated per flushed file.

For details on the features, assumptions, design, data key management, universe keys, key rotations, master failures, and adding a node, see [Encryption At Rest in YugabyteDB](https://github.com/yugabyte/yugabyte-db/blob/master/architecture/design/docdb-encryption-at-rest.md).

## Enable encryption at rest during universe creation

To enable encryption at rest during universe creation:

1. Open the Yugabyte Platform console and click **Universes**.
2. Click **Create Universe**. The **Universe Configuration** page appears.
3. After you select a provider, the **Instance Configuration** section expands to show more options.
4. Select the **Enable Encryption at Rest** option. The **Key Management Service Config** option appears.
5. Select your KMS configuration from the **Key Management Service Config** drop-down list. The list displays only preconfigured KMS configurations. If you need to create one, see [Create a KMS configuration](../create-kms-config).
6. Continue with your universe creation, then click **Create**.

To verify that encryption at rest has been successfully configured:

1. Open the Yugabyte Platform console and go to the universe.
2. Select the **Nodes** tab.
3. On one of the nodes, click **Master** under the **PROCESSES** column. The overview page appears.
4. To the right of **Replication Factor**, click **See full config**. The **Current Cluster Config** page appears.
5. Verify that the configuration includes the following `encryption_info` section with the correct values:

    ```json
    encryption_info {
      			encryption_enabled: true,
    Universe_key_registry_encoded: ".*",
    Key_in_memory: true,
    latest_version_id: ".*",
    }
    ```

6. Go to the **Security Object** page for the Equinix SmartKey account that was used to create the KMS configuration. A security object should exist with a name matching the universe UUID that was configured to use encryption at rest with the SMARTKEY configuration (defaults to AES 256).

    - **Equinix SmartKey:** Once the universe has started being created, a security object will be created with SmartKey through REST API with the universe UUID as the name of the security object. This object will then be exported to the Yugabyte Platform as the universe key. Once received, the Yugabyte Platform sends the key to all masters to persist. Yugabyte Platform does not persist any information about the security object beyond a reference to the object `kId`. At this point, data within the universe will be encrypted at rest using data keys encrypted with the aforementioned universe key generated through the selected KMS configuration.
    - **AWS KMS:** Once the universe has been created with encryption at rest enabled, the platform will persist the ciphertext of the universe key (because AWS does not persist any CMK-generated data keys themselves) and will request the plaintext of the universe key from AWS KMS using the KMS configuration whenever it needs to provide the universe key to the master nodes.

## Enable encryption at rest on an existing universe

To enable encryption at rest on an existing universe:

1. Open the Yugabyte Platform console and click **Universes**.
2. Select the universe you want to enable.
3. Click the **More** drop-down list and select **Manage Encryption Keys**. The **Manage Keys** dialog appears.
4. Select **Enable Encryption-at-Rest for <UNIVERSE_NAME>**. When enabled, the **Key Management Service Config** option appears.
5. Select your KSM configuration from the **Key Management Service Config** drop-down list. The list displays only preconfigured KMS configurations. If you need to create one, see [Create a KMS configuration](../create-kms-config).
6. Verify that the `EnableEncryptionAtRest` task completed successfully.

## Back up and restore data from an encrypted at rest universe

When you back up and restore universe data with encryption at rest enabled, Yugabyte Platform requires a key management service (KMS) configuration to manage backing up and restoring encrypted universe data. Because of the possibility that you will need to restore data to a different universe that might have a different universe key, Yugabyte Platform ensures that all encrypted backups include a metadata file (that includes a list of key IDs in the source's universe key registry). When restoring your universe data, Yugabyte Platform uses the selected KMS configuration to consume the universe key ID and then retrieve the universe key value for each key ID in the metadata file. Each of these keys are then sent to the destination universe to augment or build the universe key registry there.

## Rotate the universe keys used for encryption at rest

Enabling encryption and rotating a new key works in two steps:

1. Add the new universe key ID and key data to all the in-memory state of masters.
2. Issue a cluster configuration change to enable encryption with the new key.

The cluster configuration change does the following:

- Decrypts the universe key registry with the previous latest universe key.
- Adds the new key to the registry.
- Updates the cluster configuration with the new latest key ID.
- Encrypts the registry with the new key.

Once encryption is enabled with a new key, only new data is encrypted with this new key. Old data will remain unencrypted, or encrypted, with an older key, until compaction churn triggers a
re-encryption with the new key.

To rotate the universe keys, perform the following steps:

1. Open the Yugabyte Platform console and go the the universe.
2. Click the **More** drop-down list and select **Manage Encryption Keys**. The **Manage Keys** window appears.
3. Select **Rotate Key?** and then click **Submit**.

## Disable encryption at rest

To disable encryption at rest:

1. Open the Yugabyte Platform console and go the the universe.
2. Click the **More** drop-down list and select **Manage Encryption Keys**. The **Manage Keys** window appears.
3. Deselect **Enable Encryption-at-Rest for <UNIVERSE_NAME>** and click **Submit**.
4. To verify that encryption at rest is disabled, check the current cluster configuration for each node to see that it contains `encryption_enabled: false`.
