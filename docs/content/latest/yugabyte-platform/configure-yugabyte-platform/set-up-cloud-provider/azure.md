---
title: Configure the Microsoft Azure cloud provider
headerTitle: Configure the Microsoft Azure cloud provider
linkTitle: Configure the cloud provider
description: Configure the Microsoft Azure cloud provider.
aliases:
  - /latest/deploy/enterprise-edition/configure-cloud-providers/azure
menu:
  latest:
    identifier: set-up-cloud-provider-3-azure
    parent: configure-yugabyte-platform
    weight: 20
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/aws" class="nav-link">
      <i class="fab fa-aws"></i>
      AWS
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/gcp" class="nav-link">
      <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/azure" class="nav-link active">
      <i class="icon-azure" aria-hidden="true"></i>
      &nbsp;&nbsp; Azure
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/vmware-tanzu" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      VMware Tanzu
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/on-premises" class="nav-link">
      <i class="fas fa-building"></i>
      On-premises
    </a>
  </li>

</ul>


This page details how to configure an Azure cloud provider for YugabyteDB clusters using the Yugabyte Platform console.


## Prerequisites

You need to provide your cloud provider credentials - Subscription ID, Tenant ID, Application - ClientID/ClientSecret, Resource Group, Virtual Private Network on the Yugabyte Platform console. Yugabyte Platform uses the credentials to automatically provision and deprovision YugabyteDB instances.
You will be able to see all the resources managed by Yugabyte Platform in your resource group, which will include - Virtual Machines, Network Interface, Network Security Groups, Public IP addresses, and Disks  

## Configure Azure


![Prepare Azure cloud to install Yugabyte Platform](/images/yb-platform/install/azure/platform-azure-prepare-cloud-env-4.png)

#### Provider name
This is an internal tag used for organizing your providers, so you know where you want to deploy your YugabyteDB universes.

#### Client ID
This is the client Id of an application registered in your Azure Active Directory.

#### Client Secret
This is the client secret of an application registered in your Azure Active Directory.

#### Tenant ID
This Azure Active Directory tenant ID which belongs to an active subscription. You can find your tenant ID- https://docs.microsoft.com/en-us/azure/active-directory/develop/howto-create-service-principal-portal#get-tenant-and-app-id-values-for-signing-in.

#### Subscription ID
You need an Azure subscription ID for cost management. The virtual machine resources managed by the Platform will be tagged with this subscription.

#### Resource Group
Resource Group where YugabyteDB nodes compute and network resources will be created. Your Azure active directory application (clientId and client secret) needs to have `Network Contributor` and `Virtual Machine Contributor` roles assigned for this resource group.

#### Virtual Private Network
Using your custom Virtual Network is supported. This allows you the highest level of customization for your network setup.

## Specify Region Info

* Select a region and provide a virtual network name from your Azure portal.
* Security group is only needed if the database VM is in a different network than the platform.
* Provide the mapping of what Subnet IDs to use for each Availability Zone you wish to be able to deploy. This is required to ensure the Yugabyte Platform can deploy nodes in the correct network isolation that you desire in your environment.



![Prepare Azure cloud to install Yugabyte Platform](/images/yb-platform/install/azure/platform-azure-prepare-cloud-env-5.png)

Hit Save, and it will take a few minutes for the cloud provider to be configured. After that, you will be ready to create a YugabyteDB universe on Azure.




