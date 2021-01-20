---
title: Create a KMS configuration using AWS KMS
headerTitle: Create a KMS configuration using AWS KMS
linkTitle: Create a KMS configuration
description: Use Yugabyte Platform to create a KMS configuration for Amazon Web Services (AWS) KMS.
aliases:
  - /stable/yugabyte-platform/security/create-kms-config
menu:
  stable:
    parent: security
    identifier: create-kms-config-1-aws-kms
    weight: 27
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="{{< relref "./aws-kms.md" >}}" class="nav-link active">
      <i class="icon-postgres" aria-hidden="true"></i>
      AWS KMS
    </a>
  </li>

  <li >
    <a href="{{< relref "./equinix-smartkey.md" >}}" class="nav-link">
      <i class="icon-cassandra" aria-hidden="true"></i>
      Equinix SmartKey
    </a>
  </li>

</ul>

Encryption at rest uses universe keys to encrypt and decrypt universe data keys. You can use the Yugabyte Platform console to create key management service (KMS) configurations for generating the required universe keys for one or more YugabyteDB universes. Encryption at rest in Yugabyte Platform supports the use of [AWS Key Management Service (KMS)](https://aws.amazon.com/kms/), from Amazon Web Services (AWS).

{{< note title="Note" >}}

The AWS user associated with the a KMS configuration requires the following minimum IAM KMS-related permissions:

- `kms:CreateKey`
- `kms:ListAliases`
- `kms:ListKeys`
- `kms:CreateAlias`
- `kms:DeleteAlias`
- `kms:UpdateAlias`

{{< /note >}}

To create a KMS configuration that uses AWS Key Management Service (KMS):

1. Open the Yugabyte Platform console and click **Configs**.
2. Click the **Security** tab and then click the **Encryption At Rest** tab. A list of KMS configurations appears.
3. Click **Create Config**. A new KMS configuration form appears.
4. Enter the following configuration details:

    - **Configuration Name** Enter a meaningful name for your configuration.
    - **KMS Provider** Select `AWS KMS`.
    - **Use IAM Profile** — Select to use an IAM profile attached to an EC2 instance running YugabyteDB. For more information, see [Using instance profiles](https://docs.aws.amazon.com/IAM/latest/UserGuide/id_roles_use_switch-role-ec2_instance-profiles.html).
    - **API Url** – Enter the URL for the API. The default is `api.amer.smartkey.io`.
    - **Secret API Key** Enter the secret API key.
    - **Access Key Id** — Enter the identifier for the access key.
    - **Access Key Id** — Enter the identifier for the secret key.
    - **Region** — Select the AWS region where the CMK used to generate universe keys will be located. **Note:** This does not need to match the region where the encrypted universe resides on Amazon Web Services (AWS).
    - **Customer Master Key ID** — Enter the identifier for the customer master key. If an identifier is not entered, a CMK ID will be auto-generated.

5. (Optional) Click **Upload CMK Policy** to select a custom policy file. The default policy used is:

    ```json
    {
        "Version": "2012-10-17",
        "Id": "key-default-1",
        "Statement": [
            {
                "Sid": "Enable IAM User Permissions",
                "Effect": "Allow",
                "Principal": {
                    "AWS": "arn:aws:iam::<AWS_ACCOUNT_ID>:root"
                },
                "Action": "kms:*",
                "Resource": "*"
            },
            {
                "Sid": "Allow access for Key Administrators",
                "Effect": "Allow",
                "Principal": {
                    "AWS": "arn:aws:iam::<AWS_ACCOUNT_ID>:[user|role]{1}/[<USER_NAME>|<ROLE_NAME>]{1}"
                },
                "Action": "kms:*",
                "Resource": "*"
            }
        ]
    }

    ```

6. Click **Save**. Your new configuration should appear in the list of configurations. A saved KMS configuration can only be deleted if it is not in use by any existing universes.

7. (Optional) To confirm the information is correct, click **Show details**. Note that sensitive configuration values are displayed partially masked.
