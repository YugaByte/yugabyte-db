---
title: Manual deployment of YugabyteDB clusters
headerTitle: Manual deployment
linkTitle: Manual deployment
description: Deploy a YugabyteDB cluster in a single region or data center with a multi-zone/multi-rack configuration.
headcontent: Instructions for manually deploying YugabyteDB.
image: /images/section_icons/deploy/manual-deployment.png
aliases:
  - /deploy/multi-node-cluster/
  - /latest/deploy/multi-node-cluster/
  - /deploy/manual-deployment/
menu:
  latest:
    identifier: deploy-manual-deployment
    parent: deploy
    weight: 610
---

This section covers the generic manual deployment of a YugabyteDB cluster in a single region or data center with a multi-zone/multi-rack configuration. Note that single zone configuration is a special case of multi-zone where all placement related flags are set to the same value across every node. 

<p>

For AWS deployments specifically, a <a href="../public-clouds/aws#manual-deployment">step-by-step guide</a> to deploying a YugabyteDB cluster is also available. These steps can be easily adopted for on-premises deployments or deployments in other clouds.

<div class="row">
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="./system-config/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/system.png" aria-hidden="true" />
        <div class="title">1. System configuration</div>
      </div>
      <div class="body">
          Configuration various system parameters such as ulimits correctly in order to run YugabyteDB.
      </div>
    </a>
  </div>
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="./install-software/">
      <div class="head">
        <img class="icon" src="/images/section_icons/quick_start/install.png" aria-hidden="true" />
        <div class="title">2. Install software</div>
      </div>
      <div class="body">
          Install the YugabyteDB software on each of the nodes.
      </div>
    </a>
  </div>
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="./start-masters/">
      <div class="head">
        <img class="icon" src="/images/section_icons/admin/yb-master.png" aria-hidden="true" />
        <div class="title">3. Start YB-Masters</div>
      </div>
      <div class="body">
          Start the YB-Master service.
      </div>
    </a>
  </div>
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="./start-tservers/">
      <div class="head">
        <img class="icon" src="/images/section_icons/admin/yb-tserver.png" aria-hidden="true" />
        <div class="title">4. Start YB-TServers</div>
      </div>
      <div class="body">
          Start the YB-TServer service.
      </div>
    </a>
  </div>
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="./verify-deployment/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/checklist.png" aria-hidden="true" />
        <div class="title">5. Verify deployment</div>
      </div>
      <div class="body">
          Verify the deployment.
      </div>
    </a>
  </div>
</div>
