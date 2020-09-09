---
title: Deploy on Microsoft Azure using Azure Resource Manager (ARM)
headerTitle: Microsoft Azure
linkTitle: Microsoft Azure
description: Deploy YugabyteDB on Microsoft Azure using Azure Resource Manager (ARM).
block_indexing: true
menu:
  v2.1:
    identifier: deploy-on-azure-1-azure-arm
    parent: public-clouds
    weight: 650
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/public-clouds/azure/azure-arm" class="nav-link active">
      <i class="icon-shell"></i>
      Azure ARM template
    </a>
  </li>
  <li >
    <a href="/latest/deploy/public-clouds/azure/aks" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Azure Kubernetes Service (AKS)
    </a>
  </li>
  <li>
    <a href="/latest/deploy/public-clouds/azure/terraform" class="nav-link">
      <i class="icon-shell"></i>
      Terraform
    </a>
  </li>
</ul>

<br/>

<a href="https://portal.azure.com/#create/Microsoft.Template/uri/https%3A%2F%2Fraw.githubusercontent.com%2Fyugabyte%2Fazure-resource-manager%2Fmaster%2Fyugabyte_deployment.json" target="_blank">
    <img src="https://raw.githubusercontent.com/Azure/azure-quickstart-templates/master/1-CONTRIBUTION-GUIDE/images/deploytoazure.png"/>
</a>
<a href="http://armviz.io/#/?load=https%3A%2F%2Fraw.githubusercontent.com%2Fyugabyte%2Fazure-resource-manager%2Fmaster%2Fyugabyte_deployment.json" target="_blank">
    <img src="https://raw.githubusercontent.com/Azure/azure-quickstart-templates/master/1-CONTRIBUTION-GUIDE/images/visualizebutton.png"/>
</a>

## Prerequisites

1. Create a resource group for our YugabyteDB deployment.

- Login into your [Azure portal](https://portal.azure.com/).
- Click Resource groups from the menu of services to access the Resource Groups blade. You will see all the resource groups in your subscription listed in tblade.
- Click Add (+) to create a new resource group. The Create Resource Group blade appears.
- Provide the needed information for the new resource group.
- Click Create. The resource group might take a few seconds to create. Once it is created, you see the resource group on the Azure portal dashboard.

2. Create an SSH key for our user to get access to deployed YugabyteDB VMs.

- Open the terminal on your local computer.
- Run the following command.

    ```
    ssh-keygen
    ```

- The utility prompts you to select a location for the keys. By default, the keys are stored in the `~/.ssh` directory with the file names `id_rsa` for the private key and `id_rsa.pub` for the public key. Using the default locations allows your SSH client to automatically find your SSH keys when authenticating — press **ENTER** to accept them.

  ```
  Generating a public/private RSA key pair.
  Enter file in which to save the key (/home/username/.ssh/id_rsa):
  ```

- Once you select a location for the key, you’ll be prompted to enter an optional passphrase which encrypts the private key file on disk.

  ```
  Created directory '/home/username/.ssh'.
  Enter passphrase (empty for no passphrase):
  Enter same passphrase again:
  ```

- After this, you will have a public and private key that you can use to authenticate YugabyteDB VMs.

    ```
    Your identification has been saved in /home/username/.ssh/id_rsa.
    Your public key has been saved in /home/username/.ssh/id_rsa.pub.
    The key fingerprint is:
    a9:49:EX:AM:PL:E3:3e:a9:de:4e:77:11:58:b6:90:26 username@203.0.113.0
    The key's randomart image is:
    +--[ RSA 2048]----+
    |     ..o         |
    |   E o= .        |
    |    o. o         |
    |        ..       |
    |      ..S        |
    |     o o.        |
    |   =o.+.         |
    |. =++..          |
    |o=++.            |
    +-----------------+
    ```

## Deploy using Azure Cloud Shell

- Launch Cloud Shell.
<a href="https://shell.azure.com" target="_blank">
    <img src="https://shell.azure.com/images/launchcloudshell.png"/>
</a>

- Clone this repository.

    ```
    $ git clone https://github.com/yugabyte/azure-resource-manager.git
    ```

- Change the current directory to the cloned GitHub repository directory

    ```
      $ cd azure-resource-manager
    ```

- Use Azure CLI command to create deployments. <br/>

    ```
     $ az group deployment create --resource-group <Your-Azure-Resource-Group> --template-file yugabyte_deployment.json --parameters ClusterName='<Your-Cluster-Name>'  SshUser='<Your-SSH-USER>' YBVersion='2.0.6.0' SshKeypair='<Your-SSH-USER-PublicKey-File-Contents>'
     ```

- Once the deployment creation is complete, you can describe it as shown below.

    ```
    $ az group deployment show -g <Your-Azure-Resource-Group> -n <Your-Deployment-Name> --query properties.outputs
    ```
    
    In the output, you will get the YugabyteDB admin URL, JDBC URL, YSQL, YCQL and YEDIS connection string. You can use YugabyteDB admin URL to access admin portal.

## Deploy using Azure Portal

- Clone this repository locally.

    ```
    $ git clone https://github.com/yugabyte/azure-resource-manager.git
    ```

- First create a resource group, to create a new resource group, select **Resource groups** from the [Azure portal](https://portal.azure.com/).
- Under newly created Resource groups, select **Add**.
- In opened marketplace search for Template deployment (deploy using custom templates) and click **Create**.
- Now click **Build your own template in the editor**.
- Click **Load file** button in specify template section and upload the `yugabyte_deployment.json` file from cloned repo.
- Click **Save** at the bottom of the window.
- Now provide the required details.
- Once details are provided, then check the **Terms and Condition** checkbox and click **Purchase**.
- Once deployments get completed, you can access the YugabyteDB Admin from the URL you get in the deployment output section.
