---
title: Deploy on Amazon Elastic Kubernetes Service (EKS) using Helm Chart
linkTitle: Amazon Elastic Kubernetes Service (EKS)
description: Use Helm Chart to deploy a single-zone Kubernetes cluster on Amazon Elastic Kubernetes Service (EKS).
block_indexing: true
menu:
  v2.1:
    parent: deploy-kubernetes-sz
    name: Amazon EKS
    identifier: k8s-eks-1
    weight: 622
type: page
isTocNested: true
showAsideToc: true
---


<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/kubernetes/single-zone/eks/helm-chart" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
</ul>

Amazon EKS runs multi-zone Kubernetes clusters by default and has no support for single-zone deployments. As described on the [Amazon EKS Features](https://aws.amazon.com/eks/features/) page, the managed control plane runs in multiple availability zones by default to protect cluster administration against zone failures. Similarly, the worker nodes are automatically placed in multiple availability zones of the chosen region to protect the cluster itself from zone failures.


{{<tip title="Next step" >}}

Refer to the [Multi-zone Amazon EKS](../../../multi-zone/eks/helm-chart/) instructions for getting started with YugabyteDB on Amazon EKS.

{{< /tip >}}



