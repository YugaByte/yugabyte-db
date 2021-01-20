---
title: Prepare the Kubernetes environment
headerTitle: Prepare the Kubernetes environment
linkTitle: Prepare the environment
description: Prepare the Kubernetes environment for Yugabyte Platform.
aliases:
  - /latest/deploy/enterprise-edition/prepare-environment/
menu:
  latest:
    parent: install-yugabyte-platform
    identifier: prepare-environment-4-kubernetes
    weight: 55
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/aws" class="nav-link">
      <i class="icon-aws" aria-hidden="true"></i>
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
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/kubernetes" class="nav-link active">
      <i class="icon-aws" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/on-premises" class="nav-link">
      <i class="icon-aws" aria-hidden="true"></i>
      On-premises
    </a>
  </li>

</ul>

The Yugabyte Platform Helm chart has been tested using the following software versions:

- Kubernetes 1.10 or later.
- Helm 3.0 or later.
- Ability to pull Yugabyte Platform Docker image from quay.io repository


Before installing the YugbyteDB Admin Console, verify you have the following:

- A Kubernetes cluster configured with [Helm](https://helm.sh/).
- A Kubernetes node with minimum 4 CPU core and 15 GB RAM can be allocated to Yugabyte Platform.
- A Kubernetes secret obtained from [Yugabyte](https://www.yugabyte.com/platform/#request-trial-form).

To confirm that `helm` is configured correctly, run the following command:

```sh
$ helm version
```

The output should be similar to the following:

```
version.BuildInfo{Version:"v3.2.1", GitCommit:"fe51cd1e31e6a202cba7dead9552a6d418ded79a", GitTreeState:"clean", GoVersion:"go1.13.10"}
```
