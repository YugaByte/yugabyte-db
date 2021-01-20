---
title: Create a KMS configuration using Equinix SmartKey
headerTitle: Create a KMS configuration using Equinix SmartKey
linkTitle: Create a KMS configuration
description: Use Yugabyte Platform to create a KMS configuration using Equinix SmartKey.
menu:
  stable:
    parent: security
    identifier: create-kms-config-2-smartkey-kms
    weight: 27
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./aws-kms" >}}" class="nav-link">
      <i class="icon-postgres" aria-hidden="true"></i>
      AWS KMS
    </a>
  </li>

  <li >
    <a href="{{< relref "./equinix-smartkey" >}}" class="nav-link active">
      <i class="icon-cassandra" aria-hidden="true"></i>
      Equinix SmartKey
    </a>
  </li>

</ul>

Encryption at rest uses universe keys to encrypt and decrypt universe data keys. You can use the Yugabyte Platform console to create key management service (KMS) configurations for generating the required universe keys for one or more YugabyteDB universes. Encryption at rest in Yugabyte Platform supports the use of [Equinix SmartKey](https://www.equinix.com/services/edge-services/smartkey/) as a key management service (KMS).

To create a KMS configuration with Equinix SmartKey:

1. Open the Yugabyte Platform console and click **Configs**.
2. Click the **Security** tab and then click the **Encryption At Rest** tab. A list of KMS configurations appears.
3. Click **Create Config**. A new KMS configuration dialog appears.
4. Enter the following configuration details:

    - **Configuration Name** — Enter a meaningful name for your configuration.
    - **KMS Provider** — Select `Equinix SmartKey`.
    - **API Url** – Enter the URL for the API. The default is `api.amer.smartkey.io`.
    - **Secret API Key** — Enter the secret API key.

5. Click **Save**. The new KMS configuration should appear in the list of existing configurations. **Note:** A saved KMS configuration can only be deleted if it is not in use by any existing universes.

6. (Optional) To confirm the information is correct, click **Show details**. Note that sensitive configuration values are displayed partially masked.
