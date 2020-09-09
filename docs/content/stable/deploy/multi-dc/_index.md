---
title: Multi-DC deployments
headerTitle: Multi-DC deployment
linkTitle: Multi-DC deployments
description: Deploy YugabyteDB across multiple data centers or cloud regions
headcontent: Deploy YugabyteDB across multiple data centers (DC).
image: /images/section_icons/explore/planet_scale.png
block_indexing: true
menu:
  stable:
    identifier: multi-dc
    parent: deploy
    weight: 631
---

YugabyteDB is a geo-distributed SQL database that can be easily deployed across multiple data centers (DCs) or cloud regions. There are two primary configurations for such multi-DC deployments. 
<p>
First configuration uses a single cluster stretched across 3 or more data centers with data getting automatically sharded across all data centers. This configuration is default for <a href="../../architecture/docdb/">Spanner-inspired databases</a> like YugabyteDB. Data replication across data centers is synchronous and is based on the Raft consensus protocol. This means writes are globally consistent and reads are either globally consistent or timeline consistent (when application clients use follower reads). Additionally, resilience against data center failures is fully automatic. However, this configuration has the potential to incur Wide Area Network (WAN) latency in the write path if the data centers are geographically located far apart from each other and are connected through the shared/unreliable Internet. 
<p>
For users not requiring global consistency and automatic resilience to data center failures, the above WAN latency can be eliminated altogether through the second configuration where two independent, single-DC clusters are connected through asynchronous replication based on <a href="../../architecture/cdc-architecture/"> Change Data Capture</a>. 
<p>
<a href="https://blog.yugabyte.com/9-techniques-to-build-cloud-native-geo-distributed-sql-apps-with-low-latency/" target="_blank">9 Techniques to Build Cloud-Native, Geo-Distributed SQL Apps with Low Latency</a> highlights the various multi-DC deployment strategies for a distributed SQL database like YugabyteDB. Note that YugabyteDB is the only Spanner-inspired distributed SQL database to support a 2DC deployment.
<p>

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="3dc-deployment/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/planet_scale.png"  aria-hidden="true" />
        <div class="title">Three+ data centers (3DC)</div>
      </div>
      <div class="body">
        Deploy a single cluster across 3 or more DCs with global consistency (based on synchronous replication using Raft).
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="../kubernetes/multi-cluster/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/planet_scale.png"  aria-hidden="true" />
        <div class="title">Three+ data centers (3DC) on K8S</div>
      </div>
      <div class="body">
        Deploy a single cluster across 3 or more Kubernetes clusters, each deployed in a different DC/region.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="2dc-deployment/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/planet_scale.png"  aria-hidden="true" />
        <div class="title">Two data centers (2DC)</div>
      </div>
      <div class="body">
        Deploy 2 clusters in 2 DCs is master-follower or multi-master configuration (based on asynchronous replication using CDC).
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="read-replica-clusters/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/planet_scale.png" aria-hidden="true" />
        <div class="title">Read replica clusters</div>
      </div>
      <div class="body">
        Deploy a read replica cluster to serve low-latency reads from a remote DC.
      </div>
    </a>
  </div>


</div>
