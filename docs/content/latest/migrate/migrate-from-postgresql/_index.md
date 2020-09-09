---
title: Migrate PostgreSQL data and applications
headerTitle: Migrate from PostgreSQL
linkTitle: Migrate from PostgreSQL
description: Steps for migrating PostgreSQL data and applications to YugabyteDB.
image: /images/section_icons/develop/learn.png
menu:
  latest:
    identifier: migrate-from-postgresql
    parent: migrate
    weight: 730
---

The steps below cover how to manually migrate PostgreSQL data and applications to YugabyteDB. The sections below assume that you have read [Migration process overview](../migration-process-overview). 

{{< note title="Note" >}}

There are a number of tools that can be used to automate the entire migration from PostgreSQL, which will be covered in separate sections.

{{< /note >}}


<div class="row">

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="migrate-schema/">
      <div class="head">
        <div class="icon">
          <i class="icon-database-alt2"></i>
        </div>
        <div class="title">Migrate a DDL schema</div>
      </div>
      <div class="body">
        Migrate your DDL schema from PostgreSQL to YugabyteDB.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="migrate-application/">
      <div class="head">
        <div class="icon">
          <i class="icon-database-alt2"></i>
        </div>
        <div class="title">Migrate a PostgreSQL application</div>
      </div>
      <div class="body">
        Migrate a PostgreSQL application to YugabyteDB.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="export-data/">
      <div class="head">
        <div class="icon">
          <i class="icon-database-alt2"></i>
        </div>
        <div class="title">Export PostgreSQL data</div>
      </div>
      <div class="body">
        Export data from PostgreSQL for importing into YugabyteDB.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="prepare-cluster/">
      <div class="head">
        <div class="icon">
          <i class="icon-database-alt2"></i>
        </div>
        <div class="title">Prepare a cluster</div>
      </div>
      <div class="body">
        Prepare your YugabyteDB cluster for data import.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="import-data/">
      <div class="head">
        <div class="icon">
          <i class="icon-database-alt2"></i>
        </div>
        <div class="title">Import PostgreSQL data</div>
      </div>
      <div class="body">
        Import PostgreSQL data into a YugabyteDB cluster.
      </div>
    </a>
  </div>

  <div class="col-12 col-md-6 col-lg-12 col-xl-6">
    <a class="section-link icon-offset" href="verify-migration/">
      <div class="head">
        <div class="icon">
          <i class="icon-database-alt2"></i>
        </div>
        <div class="title">Verify the migration</div>
      </div>
      <div class="body">
        Verify the migration to YugabyteDB was successful.
      </div>
    </a>
  </div>

</div>
