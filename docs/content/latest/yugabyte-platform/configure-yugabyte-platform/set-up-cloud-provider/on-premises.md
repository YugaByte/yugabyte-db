---
title: Configure the on-premises cloud provider
headerTitle: Configure the on-premises cloud provider
linkTitle: Configure the cloud provider
description: Configure the on-premises cloud provider.
aliases:
  - /latest/deploy/enterprise-edition/configure-cloud-providers/onprem
menu:
  latest:
    identifier: set-up-cloud-provider-6-on-premises
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
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/azure" class="nav-link">
      <i class="icon-azure" aria-hidden="true"></i>
      Azure
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
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/on-premises" class="nav-link active">
      <i class="fas fa-building"></i>
      On-premises
    </a>
  </li>

</ul>

This page details how to configure the Onprem cloud provider for YugabyteDB using the Yugabyte Platform console. If no cloud providers are configured, the main Dashboard page highlights that you need to configure at least one cloud provider.


![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-0.png)

## Step 1. Configuring the on-premises provider


### On-premise Provider Info {#on-premise-provider-info}


![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-1.png)

#### Provider name

This is an internal tag used for organizing your providers, so you know where you want to deploy your YugabyteDB universes.

#### SSH User

To provision on-prem nodes with YugabyteDB, the Yugabyte Platform requires SSH access to these nodes. This user needs to have sudo permissions to complete a few tasks, which are explained in the prerequisite section.

#### SSH Port

Port number of ssh client connections. 

#### Manually Provision Nodes

If you choose to manually set up your database nodes, then set this flag to true otherwise, the Yugabyte Platform will use the sudo user to set up DB nodes. For manual provisioning, you will be prompted to execute a python script at a later stage

{{< note title="Note" >}}
If any of the items from this checklist are true, you need to provision the nodes manually.
*   Pre-provisioned `yugabyte:yugabyte` user + group
*   Sudo user requires a password.
{{< /note >}}

#### SSH Key
Ensure that the SSH key is pasted correctly (Supported format is RSA).

#### Air Gap install 
If enabled, the installation will run in an air-gapped mode without expecting any internet access.

#### Use Hostnames
Indicates if nodes are expected to use DNS or IP addresses. If enabled, then all internal communication will use DNS resolution.

#### Desired Home Directory (Optional)
Specifies the home directory of yugabyte user. The default value is /home/yugabyte.

#### Node Exporter Port
This is the port number (default value 9300) for the Node Exporter. You can override this to specify a different port.

#### Install Node Exporter
Whether to install or skip installing Node Exporter. You can skip this step if you have Node Exporter already installed on the nodes. Ensure you have provided the correct port number for skipping the installation. 

#### Node Exporter User
You can override the default prometheus user. This is useful when a user is pre-provisioned (in case user creation is disabled) on nodes. If overridden, the installer will check if the user exists and will create the user if it doesn't. 

### Provision the YugabyteDB nodes {#provision-the-yugabytedb-nodes}

Follow the steps below to provide node hardware configuration (CPU, memory, and volume information)

![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-2.png)

#### Machine Type

This is an internal user-defined tag used as an identifier in the “Instance Type” universe field.

#### Number of cores

This is the number of cores assigned to a node.

#### Mem Size (GB)

This is the memory allocation of a node.

#### Vol size (GB)

This is the disk volume of a node.

#### Mount Paths

For mount paths, use a mount point with enough space to contain your node density. Use `/data`. If you have multiple drives, add these as a comma-separated list: `/mnt/d0,/mnt/d1`


### Region and Zones

Follow the steps below to provide the location of DB nodes. All these fields are user-defined, which will be later used during the universe creation.


![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-3.png)

## Step 2. Provision the YugabyteDB nodes

After finishing the cloud provider configuration, click on “Manage Instances” to provision as many nodes as your application requires:


### Manage Instances

![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-4.png)

1. Click on “Add Instances” to add the YugabyteDB node. You can use DNS names or IP addresses when adding instances.
2. Instance ID is an optional user-defined identifier.

![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-5.png)

### Run the pre-provisioning script

{{< note title="Note" >}}
This step is only required if you set “Manually Provision Nodes” to true otherwise, you need to skip this step.
{{< /note >}}

Follow these steps for manually provisioning each node by executing the pre-provisioning python script. 


![Configure On-Premises Cloud Provider](/images/ee/onprem/configure-onprem-6.png)

1. Login (ssh) to the Platform virtual machine
2. Access the docker `yugaware `container

    ```
    > sudo docker exec -it yugaware bash
    ```


3. Copy/paste the python script prompted in the UI and substitute for a node IP address and mount points. 
    1. (Optional) Use `--ask_password `flag if sudo user requires password authentication


```
bash-4.4# /opt/yugabyte/yugaware/data/provision/9cf26f3b-4c7c-451a-880d-593f2f76efce/provision_instance.py --ip 10.9.116.65 --mount_points /data --ask_password
Executing provision now for instance with IP 10.9.116.65...
SUDO password:
```


Wait for the script to finish with the SUCCESS status.


4. Repeat step 3 for every node that will participate in the universe

You’re finished configuring your on-premises cloud provider, and now you can proceed to universe creation. 
