---
title: Prepare the private cloud (on-premise) environment
headerTitle: Prepare the private cloud (on-premise) environment
linkTitle: Prepare the environment
description: Prepare the private cloud (on-premise) environment for the Yugabyte Platform.
menu:
  latest:
    identifier: prepare-environment-5-private-cloud
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
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/azure" class="nav-link">
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
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/on-premises" class="nav-link active">
      <i class="fas fa-building" aria-hidden="true"></i>
      On-premises
    </a>
  </li>

</ul>


## Platform Node Preparation

*   Hardware requirements
    *   Nodes: 1 Virtual Machine 
    *   Cores: 8 Cores
    *   RAM: 16 GB
*   Storage Disk:  100GB (minimum) (confirm SSD)
*   Docker Engine: supported versions 1.7.1 to 19.03.8-ce. If not installed, see Installing Docker in [airgapped](https://www.replicated.com/docs/kb/supporting-your-customers/installing-docker-in-airgapped/).
*   The following ports should be open on the Yugabyte Platform host:
    *   8800 – HTTP access to the Replicated UI
    *   80 – HTTP access to the Yugabyte Platform console
    *   443 - HTTPs access to the Yugabyte Platform console
    *   22 – SSH
*   Ability to connect from the Yugabyte Platform host to all YugabyteDB data nodes. 
