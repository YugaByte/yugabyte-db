---
title: Configure the VMware Tanzu provider
headerTitle: Configure the VMware Tanzu provider
linkTitle: Configure the cloud provider
description: Configure the VMware Tanzu provider.
aliases:
  - /deploy/pivotal-cloud-foundry/
  - /latest/deploy/pivotal-cloud-foundry/
menu:
  latest:
    identifier: set-up-cloud-provider-4-vmware-tanzu
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
    <a href="/latest/yugabyte-platform/configure-yugabyte-platform/set-up-cloud-provider/vmware-tanzu" class="nav-link active">
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

This tutorial walks through the steps to a create service instance of YugabyteDB in VMware Tanzu, previously the Pivotal Cloud Foundry ().  

## Prerequisites

Before creating the Service Instance, you need to have YugabyteDB tile installed in your PCF marketplace. Follow the instructions[here](https://docs.pivotal.io/partners/yugabyte-db/). Also in your Yugabyte Platform instance that you brought up in your PCF environment you have configured cloud provider. 

After installing the tile, you have two ways to create a YugabyteDB instance, one is through the App Manager UI, and other is through Cloud Foundry CLI (cf).

## Using PCF App Manager

In your PCF App manager, go to marketplace and pick YugabyteDB, it will present you with different service plans, each service plan has a small description on what the resource requirements and what it is the intended environment.

![Yugabyte Service Plans](/images/deploy/pivotal-cloud-foundry/service-plan-choices.png)

Once you pick the service plan you would be provided with the service instance configuration screen as below ![App Manager Config](/images/deploy/pivotal-cloud-foundry/apps-manager-config.png)

## Using Cloud Foundry (cf) CLI

You can view the marketplace and plan description in the Cloud Foundry (`cf`) CLI by executing the below command.

```sh
$ cf marketplace -s yugabyte-db
```

you would see a table as shown below.

```
service plan   description                  free or paid
x-small        Cores: 2, Memory (GB): 4     paid
small          Cores: 4, Memory (GB): 7     paid
medium         Cores: 8, Memory (GB): 15    paid
large          Cores: 16, Memory (GB): 15   paid
x-large        Cores: 32, Memory (GB): 30   paid
```

Once you decide on the service plan you can launch the YugabyteDB service instance by executing the below command.

```sh
$ cf create-service yugabyte-db x-small yb-demo -c '{"universe_name": "yb-demo"}'
```

## Service broker override options

This section lists various override options that you can specify when creating a service instance using yugabyte-db service broker.

### Overriding cloud provider

Based on what cloud providers you have setup in your Yugabyte Platform, you can create Yugabyte service instances by providing
the overrides as below.

To provision in AWS/GCP cloud, your overrides would include the appropriate `provider_type` and `region_codes` as a array

```sh
{
 "universe_name": "cloud-override-demo",
 "provider_type": "gcp", # gcp for Google Cloud, aws for Amazon Web Service
 "region_codes": ["us-west1"] # this the comma delimited list of regions
}
```

To provision in Kubernetes, your overrides would include the appropriate `provider_type` and `kube_provider` type

```sh
{
 "universe_name": "cloud-override-demo",
 "provider_type": "kubernetes",
 "kube_provider": "gke" # gke for Google Compute Engine, pks for Pivotal Container Service (default)
}
```

### Overriding number of nodes

To override the number of nodes, just include the `num_nodes` with desired value, include this parameter with along with
other parameters for the cloud provider.

```sh
{
 "universe_name": "cloud-override-demo",
 "num_nodes": 4 # default is 3 nodes.
}
```

### Overriding replication factor

To override the replication factor, just include the `replication` with a desired value, include this parameter with along with
other parameters for the cloud provider. Make sure the replication factor is one of the following `1, 3, 5, 7`.

```sh
{
 "universe_name": "cloud-override-demo",
 "replication": 5,
 "num_nodes": 5 # since you change the replication factor to 5, you need to override the num_nodes to be 5 minimum.
}
```

### Overriding volume specifications

To override the volume specs, just include `num_volumes` with the desired value, and also the `volume_size` with the volume size
in GB for each of those volumes, lets say if you want to have 2 volumes with 100GB each, you would specify the overrides as below.

```sh
{
 "universe_name": "cloud-override-demo",
 "num_volumes": 2,
 "volume_size": 100
}
```

### Overriding the YugabyteDB software version to use

To override the YugabyteDB software version to use, just include `yb_version` with the desired value. Just make sure that particular
version exists in Yugabyte Platform.

```sh
{
 "universe_name": "cloud-override-demo",
 "yb_version": "1.1.6.0-b4"
}
```
