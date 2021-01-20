---
title: Back up and restore universes
headerTitle: Back up and restore universes
linkTitle: Back up and restore universes
description: Use Yugabyte Platform to back up and restore YugabyteDB universe data.
image: /images/section_icons/manage/backup.png
headcontent: Use Yugabyte Platform to back up and restore YugabyteDB universes and data.
menu:
  stable:
    parent: yugabyte-platform
    identifier: back-up-restore-universes
weight: 645
---

Yugabyte Platform can create a YugabyteDB universe with many instances (VMs, pods, machines, etc., provided by IaaS), logically grouped together to form one logical distributed database. Each universe includes one or more clusters. A universe is comprised of one primary cluster and, optionally, one or more read replica clusters. All instances belonging to a cluster run on the same type of cloud provider instance type.

<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="configure-backup-storage/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/backup.png" aria-hidden="true" />
        <div class="title">Configure backup storage</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to configure the storage location for your universe data.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="back-up-universe-data/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/backup.png" aria-hidden="true" />
        <div class="title">Back up universe data</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to restore universe data.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="restore-universe-data/">
      <div class="head">
        <img class="icon" src="/images/section_icons/manage/backup.png" aria-hidden="true" />
        <div class="title">Restore universe data</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to restore universe data.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="schedule-data-backups/">
      <div class="head">
        <img class="icon" src="/images/section_icons/explore/high_performance.png" aria-hidden="true" />
        <div class="title">Schedule universe data backups</div>
      </div>
      <div class="body">
        Use Yugabyte Platform to schedule backups of universe data.
      </div>
    </a>
  </div>

</div>
