---
title: Deploy the Yugabyte Platform for mission-critical deployments
headerTitle: Deploy Yugabyte Platform
linkTitle: Deploy
description: Use Yugabyte Platform to deploy and manage mission-critical YugabyteDB clusters.
headcontent:
image: /images/section_icons/deploy/enterprise.png
aliases:
  - /latest/deploy/enterprise-edition/
section: YUGABYTE PLATFORM
menu:
  latest:
    identifier: deploy-yugabyte-platform
    parent: yugabyte-platform
    weight: 638
---

The Yugabyte Platform is best fit for mission-critical deployments, such as production or pre-production testing. The YugabyteDB Admin Console is used in a highly available mode and orchestrates and manages YugabyteDB universes, or clusters, on one or more regions (across public cloud and private on-premises data centers).

Yugabyte Platform is a containerized application that is installed and managed using <a href="https://www.replicated.com/" target="_blank">Replicated</a> for mission-critical environments (for example, production, performance, or failure mode testing). Replicated is a purpose-built tool for on-premises deployments and life cycle management of containerized applications. For environments that are not mission-critical, such as those needed for local development or simple functional testing, you can also use <a href="../../quick-start/install">YugabyteDB</a>.

<div class="row">
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="prepare-cloud-environment/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/public-clouds.png" aria-hidden="true" />
        <div class="title">Prepare cloud environment</div>
      </div>
      <div class="body">
        Prepare your cloud environment before installing the Yugabyte Platform.
      </div>
    </a>
  </div>
  
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="install-admin-console/">
      <div class="head">
        <img class="icon" src="/images/section_icons/quick_start/install.png" aria-hidden="true" />
        <div class="title">Install Yugabyte Platform</div>
      </div>
      <div class="body">
        Install the Yugabyte Platform (aka YugaWare) on any host of your choice.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="configure-admin-console/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/enterprise/console.png" aria-hidden="true" />
        <div class="title">Configure Yugabyte Platform</div>
      </div>
      <div class="body">
        Configure your Yugabyte Platform (aka YugaWare) host.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="configure-cloud-providers/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/enterprise/administer.png" aria-hidden="true" />
        <div class="title">Configure cloud providers</div>
      </div>
      <div class="body">
          Configure both public clouds and private on-premises data centers for running YugabyteDB.
      </div>
    </a>
  </div>
</div>
