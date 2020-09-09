---
title: Explore YugabyteDB sample applications
headerTitle: Explore sample applications
linkTitle: Explore sample apps
description: Explore sample applications running on YugabyteDB.
headcontent: 
image: /images/section_icons/index/develop.png
aliases:
  - /latest/quick-start/run-sample-apps/
  - /quick-start/run-sample-apps/
menu:
  latest:
    identifier: explore-sample-apps
    parent: develop
    weight: 581
---

After [creating a local cluster](../../quick-start/create-local-cluster/), follow the instructions below to run the Yugastore application.

After running Yugastore, Yugabyte recommend running the [IoT Fleet Management](../realworld-apps/iot-spark-kafka-ksql/) application. This app is built on top of YugabyteDB as the database (using the YCQL API), Confluent Kafka as the message broker, KSQL or Apache Spark Streaming for real-time analytics and Spring Boot as the application framework.

<ul class="nav nav-tabs nav-tabs-yb">
  <li >
    <a href="#macos" class="nav-link active" id="macos-tab" data-toggle="tab" role="tab" aria-controls="macos" aria-selected="true">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>
  <li>
    <a href="#linux" class="nav-link" id="linux-tab" data-toggle="tab" role="tab" aria-controls="linux" aria-selected="false">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>
  <li>
    <a href="#docker" class="nav-link" id="docker-tab" data-toggle="tab" role="tab" aria-controls="docker" aria-selected="false">
      <i class="fab fa-docker"></i>
      Docker
    </a>
  </li>
  <li >
    <a href="#kubernetes" class="nav-link" id="kubernetes-tab" data-toggle="tab" role="tab" aria-controls="kubernetes" aria-selected="false">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>
</ul>

<div class="tab-content">
  <div id="macos" class="tab-pane fade show active" role="tabpanel" aria-labelledby="macos-tab">
    {{% includeMarkdown "binary/run-sample-apps.md" /%}}
  </div>
  <div id="linux" class="tab-pane fade" role="tabpanel" aria-labelledby="linux-tab">
    {{% includeMarkdown "binary/run-sample-apps.md" /%}}
  </div>
   <div id="docker" class="tab-pane fade" role="tabpanel" aria-labelledby="docker-tab">
    {{% includeMarkdown "docker/run-sample-apps.md" /%}}
  </div>
  <div id="kubernetes" class="tab-pane fade" role="tabpanel" aria-labelledby="kubernetes-tab">
    {{% includeMarkdown "kubernetes/run-sample-apps.md" /%}}
  </div>
</div>
