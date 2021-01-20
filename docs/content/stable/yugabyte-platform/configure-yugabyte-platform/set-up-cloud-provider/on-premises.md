---
title: Configure the on-premises cloud provider
headerTitle: Configure the on-premises cloud provider
linkTitle: Configure the cloud provider
description: Configure the on-premises cloud provider.
aliases:
  - /stable/deploy/enterprise-edition/configure-cloud-providers/onprem
menu:
  stable:
    identifier: set-up-cloud-provider-6-on-premises
    parent: configure-yugabyte-platform
    weight: 20
isTocNested: true
showAsideToc: false
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="/stable/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/aws" class="nav-link">
      <i class="fab fa-aws"></i>
      AWS
    </a>
  </li>

  <li>
    <a href="/stable/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/gcp" class="nav-link">
      <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li>
    <a href="/stable/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/azure" class="nav-link">
      <i class="icon-azure" aria-hidden="true"></i>
      Azure
    </a>
  </li>

  <li>
    <a href="/stable/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li>
    <a href="/stable/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/vmware-tanzu" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      VMware Tanzu
    </a>
  </li>

  <li>
    <a href="/stable/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/on-premises" class="nav-link active">
      <i class="fas fa-building"></i>
      On-premises
    </a>
  </li>

</ul>

## Step 1. Configure the on-premises provider using the docs instructions

1. Specify the SSH user as the `yugabyte` user required for provisioning YugabyteDB nodes.
2. Ensure that the SSH key is pasted correctly (Supported format is `RSA`)
3. Keep the default home directory path (`/home/yugabyte`).
4. For mount paths, use a mount point with enough space to contain your node density. Use `/data`.  If you have multiple drives, add these as a comma-separated list: `/mnt/d0,/mnt/d1`.

## Step 2. Provision the YugabyteDB nodes

Follow the steps below to provision as many nodes as your application requires:

1. Add the YugabyteDB node IP addresses to the on-premises cloud provider using the **Manage Instances** workflow.
2. Use DNS names or IP addresses when adding instances.
3. Use [Create a multi-zone universe](../manage/create-universe-multi-zone/).

---

![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-1.png)

![On-Premises Cloud Provider Configuration in Progress](/images/ee/onprem/configure-onprem-2.png)

![On-Premises Cloud Provider Configured Successfully](/images/ee/onprem/configure-onprem-3.png)
