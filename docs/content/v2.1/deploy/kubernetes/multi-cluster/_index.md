---
title: Deploy on multiple geo-distributed Kubernetes clusters
headerTitle: Deploy on multiple Kubernetes clusters
linkTitle: Multi-cluster
description: Deploy YugabyteDB on multiple geo-distributed Kubernetes clusters.
headcontent: Deploy YugabyteDB natively on multiple Kubernetes clusters.
image: /images/section_icons/deploy/kubernetes.png
block_indexing: true
menu:
  v2.1:
    identifier: deploy-kubernetes-mc
    parent: deploy-kubernetes
    weight: 623
---

As highlighted in <a href="https://kubernetes.io/docs/setup/best-practices/multiple-zones/">Kubernetes documentation</a>, a single Kubernetes cluster can run only inside a single zone of a single region. This means workloads will be distributed across multiple nodes of a single Kubernetes cluster running in a single region. When geo-distributed SQL workloads like YugabyteDB need to run on Kubernetes, the preferred approach is to create a single YugabyteDB cluster that spans <a href="../multi-zone">multiple zones of a single Kubernetes cluster</a> or multiple geo-distributed Kubernetes clusters. This page documents the latter configuration.

<p>

<a href="https://cloud.google.com/kubernetes-engine/docs/concepts/types-of-clusters">Google Kubernetes Engine</a> can be easily configured to support global DNS across multiple Kubernetes clusters. Following instructions highlight how to deploy a 3-region YugabyteDB cluster on 3 Kubernetes clusters, each deployed in a different region. It uses the standard single-zone YugabyteDB Helm Chart to deploy one third of the nodes in the database cluster in each of the 3 clusters.

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="gke/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/gke.png" aria-hidden="true" />
        <div class="title">Google Kubernetes Engine (GKE)</div>
      </div>
      <div class="body">
        Multi-cluster deployment on GKE.
      </div>
    </a>
  </div>

</div>
