---
title: Prepare cloud environments for Yugabyte Platform
headerTitle: Prepare cloud environment for GCP
linkTitle: 1. Prepare cloud environment
description: Prepare your GCP environment for the Yugabyte Platform.
block_indexing: true
menu:
  stable:
    identifier: prepare-cloud-2-gcp
    name: Prepare cloud environment
    parent: deploy-yugabyte-platform
    weight: 669
isTocNested: true
showAsideToc: true

---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/stable/yugabyte-platform/deploy/prepare-cloud-environment/aws" class="nav-link">
      <i class="icon-aws" aria-hidden="true"></i>
      AWS
    </a>
  </li>
  <li>
    <a href="/stable/yugabyte-platform/deploy/prepare-cloud-environment/gcp" class="nav-link active">
       <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>
</ul>

A dedicated host or virtual machine (VM) is required to run the Yugabyte Platform server. For more details, see [this faq](../../../../faq/yugabyte-platform/#what-are-the-os-requirements-and-permissions-to-run-yugaware-the-yugabyte-admin-console). This page highlights the basic setup needed in order to install Yugabyte Platform.

## 1. [Optional] Create a new project

A project forms the basis for creating, enabling and using all GCP services, managing APIs, enabling billing, adding and removing collaborators, and managing permissions. You would need browse to the [GCP cloud resource manager](https://console.cloud.google.com/cloud-resource-manager) and click on create project to get started. You can follow these instructions to [create a new GCP project](https://cloud.google.com/resource-manager/docs/creating-managing-projects).

Give the project a suitable name (eg: `yugabyte-gcp`) and note the project ID (eg: `yugabyte-gcp`). You should see a dialog that looks like the screenshot below.

![Creating a GCP project](/images/ee/gcp-setup/project-create.png)

## 2. Set up a new service account

YugabyteDB Admin Console requires a service account with the appropriate permissions to provision and manage compute instances. Go to the `IAM & admin` -> `Service accounts` and click on `Create Service Account`. You can follow these instructions to [create a service account](https://cloud.google.com/iam/docs/creating-managing-service-accounts).

Fill the form with the following values:

- Service account name is `yugaware` (you can customize the name if needed).
- Set role to `Project` -> `Owner`.
- Check the box for `Furnish a new private key`, choose `JSON` option.

Here is a screenshot with the above values in the form, click create once the values are filled in.

![Service Account -- filled create form](/images/ee/gcp-setup/service-account-filled-create.png)

**NOTE**: Your browser would have downloaded the respective JSON format key. It is important to store it safely. This JSON key is needed to configure the YugabyteDB Admin Console.

## 3. Give permissions to the service account

- Find the email address associated with the service account by going to `IAM & admin` -> `Service accounts`. Copy this value. The screen should look as shown below.

![Service Account Email Address](/images/ee/gcp-setup/gcp-service-account-email.png)

- Next, browse to `IAM & admin` -> `IAM` and click on `ADD`. Add the compute admin role for this service account. A screenshot is shown below.

![Service Account Add Roles](/images/ee/gcp-setup/gcp-service-account-permissions.png)

## 4. Creating a firewall rule

In order to access the Yugabyte Platform from outside the GCP environment, you would need to enable firewall rules. You will at minimum need to:

- Access the Yugabyte Platform instance over SSH (port tcp:22)
- Check, manage, and upgrade Yugabyte Platform (port tcp:8800)
- View the YugabyteDB Admin Console (port tcp:80)

Let us create a firewall entry enabling all of that!

Go to `VPC network` -> `Firewall rules` tab:

![Firewall -- service entry](/images/ee/gcp-setup/firewall-tab.png)

**NOTE**: If this is a new project, you might see a message saying `Compute Engine is getting ready`. If so, you would need to wait for a while. Once complete, you should see the default set of firewall rules for your default network, as shown below.

![Firewall -- fresh list](/images/ee/gcp-setup/firewall-fresh-list.png)

Click on the `CREATE FIREWALL RULE` button and fill in the following.

- Enter `yugaware-firewall-rule` as the name (you can change the name if you want).
- Add a description (for example, `Firewall setup for YugabyteDB Admin Console`).
- Add a tag `yugaware-server` to the `Target tags` field. This will be used later when creating instances.
- Add the appropriate ip addresses to the `Source IP ranges` field. To allow access from any machine, add `0.0.0.0/0` but note that this is not very secure.
- Add the ports `tcp:22,8800,80` to the `Protocol and ports` field.

You should see something like the screenshot below, click `Create` next.

![Firewall -- create full](/images/ee/gcp-setup/firewall-create-full.png)

## 5. Provision instance for YugaWare

Create an instance to run YugaWare. In order to do so, go to `Compute Engine` -> `VM instances` and click on `Create`. Fill in the following values.

- Enter `yugaware-1` as the name.
- Pick a region/zone (eg: `us-west1-b`).
- Choose `4 vCPUs` (`n1-standard-4`) as the machine type.
- Change the boot disk image to `Ubuntu 16.04` and increase the boot disk size to `100GB`.
- Open the `Management, disks, networking, SSH keys` -> `Networking` tab. Add `yugaware-server` as the network tag (or the custom name you chose when setting up the firewall rules).
- Switch to the `SSH Keys` tab and add a custom public key and login user to this instance. First create a key-pair.

You can do this as shown below.

```sh
$ ssh-keygen -t rsa -f ~/.ssh/yugaware-1-gcp -C centos
```

Set the appropriate credentials for the ssh key.

```sh
$ chmod 400 ~/.ssh/yugaware-1-gcp
```

Now enter the contents of `yugaware-1-gcp.pub` as the value for this field.

[Here are the detailed instructions](https://cloud.google.com/compute/docs/instances/adding-removing-ssh-keys#metadatavalues) to create a new SSH key pair, as well as the expected format for this field (eg: `ssh-rsa [KEY_VALUE] [USERNAME]`). This is important to enable `ssh` access to this machine.


![VM instances -- filled in create](/images/ee/gcp-setup/vm-create-full.png)

Note on boot disk customization:

![VM instances -- pick boot disk](/images/ee/gcp-setup/vm-pick-boot-disk.png)

Note on networking customization:

![VM instances -- networking tweaks](/images/ee/gcp-setup/vm-networking.png)

Finally, click `Create` to launch the Yugabyte Platform server.

## 6. Connect to the Yugabyte Platform machine

From the GCP web management console, find the public IP address of the instance you just launched.

You can connect to this machine by running the following command (remember to replace `XX.XX.XX.XX` below with the ip address, and also to enter the appropriate ssh key instead of `yugaware-1-gcp`).

```sh
$ ssh -i ~/.ssh/yugaware-1-gcp centos@XX.XX.XX.XX
```
