---
title: Prepare the Azure cloud environment
headerTitle: Prepare the Azure cloud environment
linkTitle: Prepare the environment
description: Prepare the Azure environment for the Yugabyte Platform.
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
menu:
  latest:
    identifier: prepare-environment-3-azure
    parent: install-yugabyte-platform
    weight: 55
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/aws" class="nav-link">
      <i class="fab fa-aws" aria-hidden="true"></i>
      AWS
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/gcp" class="nav-link">
       <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/azure" class="nav-link active">
      <i class="icon-azure" aria-hidden="true"></i>
      &nbsp;&nbsp; Azure
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/on-premises" class="nav-link">
      <i class="fas fa-building" aria-hidden="true"></i>
      On-premises
    </a>
  </li>

</ul>


## Pre-requisites

You are going to need these details from your Azure Cloud tenant:

* Active subscription and subscription ID for cost management
* [Tenant ID](https://docs.microsoft.com/en-us/azure/active-directory/develop/howto-create-service-principal-portal#get-tenant-and-app-id-values-for-signing-in)
* You must have sufficient permissions 
    * To [register an application](https://docs.microsoft.com/en-us/azure/active-directory/develop/howto-create-service-principal-portal#permissions-required-for-registering-an-app) with your Azure AD tenant, and 
    * To [assign the application](https://docs.microsoft.com/en-us/azure/active-directory/develop/howto-create-service-principal-portal#check-azure-subscription-permissions)  roles in your Azure subscription


## Create resource group (optional) 

You can skip creating a new resource group and [use an existing one to manage Yugabyte Platform resources](
https://docs.microsoft.com/en-us/azure/azure-resource-manager/management/manage-resource-groups-portal#create-resource-groups)


## Create network security group (optional)

You can skip creating a new security group and use an existing one to manage network access

To access the Yugabyte Platform from outside the Azure environment, you would need to enable access by assigning an appropriate network security group to the Yugabyte Platform machine. You will at minimum need to:

* Access the Yugabyte Platform instance over SSH (port tcp:22)
* Check, manage, and upgrade Yugabyte Platform (port tcp:8800)
* View the Yugabyte Platform console (port tcp:80)

Yugabyte platform will provision and access database nodes in a later step; you will need to provide a virtual network where the platform needs to create the database nodes. So you would need to ensure connectivity between the platform VM  virtual network and database VMs virtual network. You may need virtual network peering based on your network configuration. Please make sure the platform can access these nodes on the database VM’s virtual network - https://docs.yugabyte.com/latest/reference/configuration/default-ports/
To create a security group that enables these, go to Network Security Groups > Add> Choose subscription > Select resource group used in the previous step > Add name and region, click Create Security Group, and then add the following values:

* For the name, enter yugaware-sg (you can change the name if you want).
* Edit inbound security rules:
    *  Add the appropriate IP addresses to the Source IP ranges field. To allow access from any machine, add 0.0.0.0/0 but note that this is not very secure.
    * Add the ports 22, 8800, and 80 to the Port Range field. The protocol selected must be TCP

## Create a service principal   

For the Yugabyte Platform to manage YugabyteDB nodes, it requires limited access to your Azure infrastructure. This can be accomplished by [registering an app](https://docs.microsoft.com/en-us/azure/active-directory/develop/quickstart-register-app) in the Azure portal so the Microsoft identity platform can provide authentication and authorization services for your application. Registering your application establishes a trust relationship between your app and the Microsoft identity platform.

Follow these steps to create the app registration:
* Sign in to the Azure portal.
* If you have access to multiple tenants, use the Directory + subscription filter in the top menu to select the tenant used in previous steps to register an application.
* Search for and select the Azure Active Directory.
* Under Manage, select App registrations, then New registration.
* Enter a Name for your application. Users of your app might see this name, and you can change it later.
* Specify who can use the application. You can use either .single or multiple tenant options.
* Don't enter anything for Redirect URI (optional) and Select Register to complete the initial app registration.


![Prepare Azure cloud to install Yugabyte Platform](/images/yb-platform/install/azure/platform-azure-prepare-cloud-env-1.png)


* When registration completes, the Azure portal displays the app registration's Overview pane, which includes its Application (client) ID. Also referred to as just client ID, this value uniquely identifies your Microsoft identity platform application.
* App authentication: Select Certificates & secrets > New client secret.
* Add a description of your client secret.
* Select a duration.
* Select Add.
* Record the secret's value to be used later for configuring the platform - it's never displayed again after you leave this page.

![Prepare Azure cloud to install Yugabyte Platform](/images/yb-platform/install/azure/platform-azure-prepare-cloud-env-2.png)

## Assign a role to the application

Access to Azure infrastructure resources in a subscription  (Virtual machines, Network configurations) is restricted by the roles assigned to your application, giving you control over which resources can be accessed and at which level. You can set the scope at the level of the subscription, resource group, or resource. Permissions are inherited to lower levels of scope. We are going to assign permissions over a resource group that was created in the previous steps

See the [Azure documentation for reference](https://docs.microsoft.com/en-us/azure/active-directory/develop/howto-create-service-principal-portal#assign-a-role-to-the-application)

* In the Azure portal, navigate to the resource group and select Access control (IAM)
* Select Add role assignment.

![Prepare Azure cloud to install Yugabyte Platform](/images/yb-platform/install/azure/platform-azure-prepare-cloud-env-3.png)

* Select  `Network Contributor` and `Virtual Machine Contributor` roles
* Select your application created in the previous step
* Select Save to finish assigning the role. You see your application in the list of users with a role for that scope.

Your service principal is set up, and now you can start using it for configuring the platform.

## Provision instance for Yugabyte Platform

Create an instance to run the Yugabyte Platform server. To do so, go to Virtual Machines>Add and Fill in the following values.

* Choose your active subscription and resource group.
* Provide a name for the virtual machine.
* Choose a region where you want to deploy the platform.
* Ignore the availability options.
* Change the disk image to Ubuntu 16.04.
* Choose “Standard_D4s_v3” - 4 CPU/16GB memory instance.
* Select the authentication type as “ssh public key.”. Pick an existing key pair (or create a new one) to access the machine. Make sure you have the ssh access key. This is important for enabling ssh access to this machine.
* Select public inbound ports based on network configuration. You can disable public access if you wish to access the instance from within a private network.
* On the disks page, you can select any OS disk type.
* Increase the data disk size to at least 100GiB by creating an “attached new disk.” 
* Continue to the next networking section and fill out the details for the virtual network and security group created in the previous steps.

Finally, click Review+create to launch the Yugabyte Platform VM.
