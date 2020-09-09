---
title: Deploy YugabyteDB on a multi-zone Kubernetes cluster
linkTitle: Multi-zone
headerTitle: Multi-zone Kubernetes
description: Deploy YugabyteDB on multi-zone Kubernetes
headcontent: Deploy YugabyteDB on a multi-zone Kubernetes cluster
image: /images/section_icons/deploy/kubernetes.png
block_indexing: true
menu:
  v2.1:
    identifier: deploy-kubernetes-mz
    parent: deploy-kubernetes
    weight: 622
---

As highlighted in <a href="https://kubernetes.io/docs/setup/best-practices/multiple-zones/">Kubernetes multi-zone documentation</a>, Kubernetes is by default a single-zone technology. This means workloads will be distributed across multiple nodes of a single Kubernetes cluster running in a single zone. Running a single Kubernetes cluster with nodes present in multiple zones of a single region is possible with additional configuration. Multi-zone Kubernetes clusters have a <a href="https://kubernetes.io/docs/setup/best-practices/multiple-zones/#limitations">list of known limitations</a> that users should be aware of.

<p>

<a href="https://docs.aws.amazon.com/eks/latest/userguide/network_reqs.html">Amazon Elastic Kubernetes Service</a> and <a href="https://cloud.google.com/kubernetes-engine/docs/concepts/types-of-clusters">Google Kubernetes Engine</a> supports multi-zone Kubernetes clusters automatically. The following instructions highlight how to deploy a 3-zone YugabyteDB cluster on a 3-zone Kubernetes cluster. Both these deployments use the standard single-zone YugabyteDB Helm Chart to deploy one third of the nodes in the database cluster in each of the 3 zones.

<p>

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="eks/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/amazon-eks.png" aria-hidden="true" />
        <div class="title">Amazon Elastic Kubernetes Service (Amazon EKS)</div>
      </div>
      <div class="body">
        Multi-zone deployment on Amazon EKS.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="gke/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/gke.png" aria-hidden="true" />
        <div class="title">Google Kubernetes Engine (GKE)</div>
      </div>
      <div class="body">
        Multi-zone deployment on GKE.
      </div>
    </a>
  </div>

</div>
