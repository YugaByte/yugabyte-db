---
title: Manage YugabyteDB universe deployments
headerTitle: Manage YugabyteDB universe deployments
linkTitle: Manage deployments
description: Manage YugabyteDB universe deployments
image: /images/section_icons/quick_start/sample_apps.png
menu:
  latest:
    identifier: manage-deployments
    parent: yugabyte-platform
    weight: 644
isTocNested: true
showAsideToc: true
---

Yugabyte Platform can create a YugabyteDB universe with many instances (VMs, pods, machines, etc., provided by IaaS), logically grouped together to form one logical distributed database. Each universe includes one or more clusters. A universe is comprised of one primary cluster and, optionally, one or more read replica clusters. All instances belonging to a cluster run on the same type of cloud provider instance type.

<div class="row">

<!--
  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="view-all-universes/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/create_universe.png" aria-hidden="true" />
        <div class="title">View all universes</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to view all YugabyteDB universes.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="view-universe-details/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/create_universe.png" aria-hidden="true" />
        <div class="title">View universe details</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to view details about a universe.
      </div>
    </a>
  </div>
-->

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="start-stop-processes/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/edit_universe.png" aria-hidden="true" />
        <div class="title">Start and stop processes</div>
      </div>
      <div class="body">
        Start and stop processes.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="add-nodes/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/system.png" aria-hidden="true" />
        <div class="title">Add a node</div>
      </div>
      <div class="body">
        Add nodes to your YugabyteDB universe.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="remove-nodes/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/manual-deployment.png" aria-hidden="true" />
        <div class="title">Remove a node</div>
      </div>
      <div class="body">
        Remove nodes from a universes.
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
        Edit configuration flags to customize your processes.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="upgrade-software/">
      <div class="head">
        <img class="icon" src="/images/section_icons/quick_start/install.png" aria-hidden="true" />
        <div class="title">Upgrade YugabyteDB software</div>
      </div>
      <div class="body">
        Upgrade YugabyteDB software.
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
        Use Yugabyte Platform to edit a universe.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="instance-tags/">
      <div class="head">
        <img class="icon" src="/images/section_icons/deploy/manual-deployment.png" aria-hidden="true" />
        <div class="title">Configure instance tags</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to create and edit instance tags.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="delete-universe/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise/edit_universe.png" aria-hidden="true" />
        <div class="title">Delete a universe</div>
      </div>
      <div class="body">
        Delete a universe that is not needed.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="migrate-to-helm3/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/enterprise.png" aria-hidden="true" />
        <div class="title">Migrate to Helm 3</div>
      </div>
      <div class="body">
        Migrate your deployment from Helm 2 to Helm 3.
      </div>
    </a>
  </div>

</div>
