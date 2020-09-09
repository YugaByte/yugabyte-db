---
title: Configure Kubernetes for YugabyteDB
headerTitle: Configure cloud providers
linkTitle: 4. Configure cloud providers
description: Configure Kubernetes for YugabyteDB deployments using the YugabyteDB Admin Console
aliases:
  - /stable/deploy/enterprise-edition/configure-cloud-providers/kubernetes
block_indexing: true
menu:
  stable:
    identifier: configure-cloud-providers-4-kubernetes
    parent: deploy-yugabyte-platform
    weight: 680
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/stable/yugabyte-platform/deploy/configure-cloud-providers/aws" class="nav-link">
      <i class="fab fa-aws"></i>
      AWS
    </a>
  </li>

  <li >
    <a href="/stable/yugabyte-platform/deploy/configure-cloud-providers/gcp" class="nav-link">
      <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li >
    <a href="/stable/yugabyte-platform/deploy/configure-cloud-providers/azure" class="nav-link">
      <i class="icon-azure" aria-hidden="true"></i>
      Azure
    </a>
  </li>

  <li >
    <a href="/stable/yugabyte-platform/deploy/configure-cloud-providers/kubernetes" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li >
    <a href="/stable/yugabyte-platform/deploy/configure-cloud-providers/vmware-tanzu" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      VMware Tanzu
    </a>
  </li>

  <li >
    <a href="/stable/yugabyte-platform/deploy/configure-cloud-providers/onprem" class="nav-link">
      <i class="fas fa-building"></i>
      On-premises
    </a>
  </li>

</ul>

This page details how to configure Kubernetes for YugabyteDB universes using the Yugabyte Platform. If no cloud providers are configured in the YugabyteDB Admin Console yet, the main Dashboard page highlights the need to configure at least one cloud provider.

![Configure Cloud Provider](/images/ee/configure-cloud-provider.png)

## Prerequisites

### Kubernetes

If you plan to run YugabyteDB nodes on Kubernetes, all you need to provide in the YugabyteDB Admin Console is your cloud provider credentials. The Yugabyte Platform will use those credentials to automatically provision and de-provision instances that run Yugabyte. An 'instance' for YugabyteDB includes a compute instance as well as local or remote disk storage attached to the compute instance.

## Configure Kubernetes credentials

## Pick appropriate k8s tab

For Kubernetes, you have two options, one is to using Pivotal Container Service or Managed Kubernetes Service, depending on what you are using click on the appropriate tab.
<img title="K8s Configuration -- Tabs" alt="K8s Configuration -- Tabs" class="expandable-image" src="/images/ee/k8s-setup/k8s-provider-tabs.png" />

Once you go to the appropriate tab, you should see a configuration form like this:

<img title="K8s Configuration -- empty" alt="K8s Configuration -- empty" class="expandable-image" src="/images/ee/k8s-setup/k8s-configure-empty.png" />

Select the Kubernetes provider type from **Type**. In the case of Pivotal Container Service, this would be default to that option.

## Configure the provider

Take note of the following for configuring your K8s provider:

- Give a meaningful name for your configuration.

- **Service Account** provide the name of the service account which has necessary access to manage the cluster, refer to [Create cluster](../../../../deploy/kubernetes/single-zone/oss/helm-chart/#create-cluster).

- **Kube Config** there are two ways to specify the kube config for an Availability Zone.
  - Specify at **provider level** in the provider form as shown above. If specified, this config file will be used for all AZ's in all regions.
  - Specify at **zone level** inside of the region form as described below, this is especially needed for **multi-az** or **multi-region** deployments.

- **Image Registry** specifies where to pull YugabyteDB image from leave this to default, unless you are hosting the registry on your end.

- **Pull Secret**, the Enterprise YugabyteDB image is in a private repo and you need to upload the pull secret to download the image, your sales representative should have provided this secret.

A filled in form looks something like this:

<img title="K8s Configuration -- filled" alt="K8s Configuration -- filled" class="expandable-image" src="/images/ee/k8s-setup/k8s-configure-filled.png" />

## Configure the region and zones

Click **Add Region** to open the modal.

- Specify a Region and the dialog will expand to show the zone form.

- **Zone**, enter a zone label, keep in mind this label should match with your failure domain zone label `failure-domain.beta.kubernetes.io/zone`

- **Storage Class** is *optional*, it takes a comma delimited value, if not specified would default to standard, please make sure this storage class exists in your k8s cluster.

- **Kube Config** is *optional* if specified at provider level or else `required`

<img title="K8s Configuration -- zone config" alt="K8s Configuration -- zone config" class="expandable-image" src="/images/ee/k8s-setup/k8s-az-kubeconfig.png" />

- `Overrides` is *optional*, if not specified Yugabyte Platform would use defaults specified inside the helm chart,

- Overrides to add Service level annotations

```yml
serviceEndpoints:
  - name: "yb-master-service"
    type: "LoadBalancer"
    annotations:
      service.beta.kubernetes.io/aws-load-balancer-internal: "0.0.0.0/0"
    app: "yb-master"
    ports:
      ui: "7000"

  - name: "yb-tserver-service"
    type: "LoadBalancer"
    annotations:
      service.beta.kubernetes.io/aws-load-balancer-internal: "0.0.0.0/0"
    app: "yb-tserver"
    ports:
      ycql-port: "9042"
      yedis-port: "6379"
      ysql-port: "5433"
```

- Overrides to disable LoadBalancer

```yml
enableLoadBalancer: False
```

- Overrides to change the cluster domain name

```yml
domainName: my.cluster
```

- Overrides to add annotations at StatefulSet level

```yml
networkAnnotation:
  annotation1: 'foo'
  annotation2: 'bar'
```

Add a new Zone by clicking **Add Zone** on the bottom left of the zone form.

Your form may have multiple AZ's as shown below.

<img title="K8s Configuration -- region" alt="K8s Configuration -- region" class="expandable-image" src="/images/ee/k8s-setup/k8s-add-region-flow.png" />

Click **Add Region** to add the region and close the modal.

Click **Save** to save the configuration. If successful, it will redirect you to the table view of all configurations.

## Next step

You are now ready to create YugabyteDB universes as outlined in the next section.
