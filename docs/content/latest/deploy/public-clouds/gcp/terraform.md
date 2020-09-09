---
title: Deploy YugabyteDB in Google Cloud Platform with Terraform
headerTitle: Google Cloud Platform
linkTitle: Google Cloud Platform
description: Use Terraform to deploy a YugabyteDB cluster in Google Cloud Platform.
aliases:
  - /latest/deploy/public-clouds/gcp/
menu:
  latest:
    identifier: deploy-in-gcp-3-terraform
    parent: public-clouds
    weight: 640
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/deploy/public-clouds/gcp/gcp-deployment-manager" class="nav-link">
      <i class="icon-shell"></i>
      Google Cloud Deployment Manager
    </a>
  </li>

  <li>
    <a href="/latest/deploy/public-clouds/gcp/gke" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Google Kubernetes Engine (GKE)
    </a>
  </li>

  <li >
    <a href="/latest/deploy/public-clouds/gcp/terraform" class="nav-link active">
      <i class="icon-shell"></i>
      Terraform
    </a>
  </li>

</ul>

## Prerequisites

1. Download and install [terraform](https://www.terraform.io/downloads.html).

2. Verify by the `terraform` command, it should print a help message that looks similar to that shown below.

```sh
$ terraform
```

```
Usage: terraform [--version] [--help] <command> [args]
...
Common commands:
    apply              Builds or changes infrastructure
    console            Interactive console for Terraform interpolations
    destroy            Destroy Terraform-managed infrastructure
    env                Workspace management
    fmt                Rewrites config files to canonical format
```

## 1. Create a terraform configuration file

* First, create a terraform file with provider details.

```
  provider "google"
  {
    # Provide your Creadentilals
    credentials = "${file("yugabyte-pcf-bc8114281026.json")}"

    # The name of your GCP project
    project = "<Your-GCP-Project-Name>"
  }
```

  **NOTE:** :- You can get credentials file by following steps given [here](https://cloud.google.com/docs/authentication/getting-started)

* Now add the Yugabyte Terraform module to your file.

```
  module "yugabyte-db-cluster" {
  source = "github.com/Yugabyte/terraform-gcp-yugabyte.git"

  # The name of the cluster to be created.
  cluster_name = "test-cluster"

   # key pair.
  ssh_private_key = "SSH_PRIVATE_KEY_HERE"
  ssh_public_key = "SSH_PUBLIC_KEY_HERE"
  ssh_user = "SSH_USER_NAME_HERE"

  # The region name where the nodes should be spawned.
  region_name = "YOUR_VPC_REGION"

  # Replication factor.
  replication_factor = "3"

  # The number of nodes in the cluster, this cannot be lower than the replication factor.
  node_count = "3"
  }
```

## 2. Create a cluster

Init terraform first if you have not already done so.

```sh
$ terraform init
```

To check what changes are going to happen in environment run the following

```sh
$ terraform plan
```

Now run the following to create the instances and bring up the cluster.

```sh
$ terraform apply
```

Once the cluster is created, you can go to the URL `http://<node ip or dns name>:7000` to view the UI. You can find the node's ip or dns by running the following:

```sh
$ terraform state show google_compute_instance.yugabyte_node[0]
```

You can access the cluster UI by going to any of the following URLs.

You can check the state of the nodes at any point by running the following command.

```sh
$ terraform show
```

## 3. Verify resources created

The following resources are created by this module:

- `module.terraform-gcp-yugabyte.google_compute_instance.yugabyte_node` The GCP VM instances.

For cluster named `test-cluster`, the instances will be named `yugabyte-test-cluster-n1`, `yugabyte-test-cluster-n2`, `yugabyte-test-cluster-n3`.

- `module.terraform-gcp-yugabyte.google_compute_firewall.Yugabyte-Firewall` The firwall rule that allows the various clients to access the YugabyteDB cluster.

For cluster named `test-cluster`, this firewall rule will be named `default-yugabyte-test-cluster-firewall` with the ports 7000, 9000, 9042 and 6379 open to all.

- `module.terraform-gcp-yugabyte.google_compute_firewall.Yugabyte-Intra-Firewall` The firewall rule that allows communication internal to the cluster.

For cluster named `test-cluster`, this firewall rule will be named `default-yugabyte-test-cluster-intra-firewall` with the ports 7100, 9100 open to all other vm instances in the same network.

- `module.terraform-gcp-yugabyte.null_resource.create_yugabyte_universe` A local script that configures the newly created instances to form a new YugabyteDB universe.

## 4. Destroy the cluster (optional)

To destroy what you just created, you can run the following command.

```sh
$ terraform destroy
```
