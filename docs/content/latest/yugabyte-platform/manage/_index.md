---
title: Manage Yugabyte Platform
headerTitle: Manage Yugabyte Platform
linkTitle: Manage
description: Use Yugabyte Platform to manage YugabyteDB universes.
image: /images/section_icons/manage/enterprise.png
headcontent: Use Yugabyte Platform's orchestration and monitoring to manage YugabyteDB universes.
aliases:
  - /latest/manage/enterprise-edition/
menu:
  latest:
    identifier: manage-yugabyte-platform
    parent: yugabyte-platform
    weight: 707
---

Yugabyte Platform can create a YugabyteDB universe with many instances (VMs, pods, machines, etc., provided by IaaS), logically grouped together to form one logical distributed database. Each universe includes one or more clusters. A universe is comprised of one primary cluster and, optionally, one or more read replica clusters. All instances belonging to a cluster run on the same type of cloud provider instance type.

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="create-universe-multi-zone/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/create_universe.png" aria-hidden="true" />
        <div class="title">Create a multi-zone universe</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to create a YugabyteDB universe spans multiple zones within one region.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="create-universe-multi-region/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/create_universe.png" aria-hidden="true" />
        <div class="title">Create a multi-region universe</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to create a YugabyteDB universe that spans multiple regions.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="edit-universe/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/edit_universe.png" aria-hidden="true" />
        <div class="title">Edit a universe</div>
      </div>
      <div class="body">
        Expand, shrink, or reconfigure a universe without downtime.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="edit-config-flags/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/edit_flags.png" aria-hidden="true" />
        <div class="title">Edit configuration flags</div>
      </div>
      <div class="body">
        Change configuration flags for a universe in a rolling manner, without any application impact.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="instance-tags/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/edit_flags.png" aria-hidden="true" />    
        <div class="title">Create and edit instance tags</div>
      </div>
      <div class="body">
        Create and edit instance tags to help manage, bill, or audit resources.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="cluster-health/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/diagnostics.png" aria-hidden="true" />
        <div class="title">Configure health checks and alerts</div>
      </div>
      <div class="body">
        Configure automatic health checking and error reporting for a universe.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="read-replicas/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/create_universe.png" aria-hidden="true" />
        <div class="title">Create a read replica cluster</div>
      </div>
      <div class="body">
        Create a YugabyteDB universe, with a primary cluster and one or more read replica clusters.
      </div>
    </a>
  </div>
  
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="backup-restore/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise.png" aria-hidden="true" />
        <div class="title">Back up and restore data</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to back up and restore universe data.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="back-up-restore-yp/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise.png" aria-hidden="true" />
        <div class="title">Back up and restore Yugabyte Platform</div>
      </div>
      <div class="body">
        Back up and restore Yugabyte Platform for disaster recovery.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="schedule-data-backup/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise.png" aria-hidden="true" />
        <div class="title">Schedule a data backup</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to perform a regular scheduled backup of universe data.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="upgrade-universe/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/upgrade_universe.png" aria-hidden="true" />
        <div class="title">Upgrade a universe</div>
      </div>
      <div class="body">
        Perform a rolling upgrade of a universe, without any application impact.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="node-actions/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/edit_universe.png" aria-hidden="true" />
        <div class="title">Node status and actions</div>
      </div>
      <div class="body">
        Node status and actions to transition them for node maintenance.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="delete-universe/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/delete_universe.png" aria-hidden="true" />
        <div class="title">Delete a universe</div>
      </div>
      <div class="body">
        Delete a universe to free up infrastructure capacity.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="migrate-to-helm3/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/create_universe.png" aria-hidden="true" />
        <div class="title">Migrate from Helm 2 to Helm 3</div>
      </div>
      <div class="body">
        Migrate YugabyteDB universes and Yugabyte Platform from Helm 2 to Helm 3.
      </div>
    </a>
  </div>

</div>
