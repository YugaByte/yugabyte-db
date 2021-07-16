---
title: Grafana Dashboard
headerTitle: Grafana Dashboard
linkTitle: Grafana Dashboard 
description: Learn about setting up the community dashboards with Prometheus data source using Grafana.
menu:
  latest:
    identifier: observability-1-macos
    parent: explore-observability
    weight: 240
isTocNested: true
showAsideToc: true
---

 <ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/observability/grafana-dashboard/macos/" class="nav-link active">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>
  </ul>
   
  You can create dashboards for your local YugabyteDB clusters using [Grafana](https://grafana.com/grafana/), an open source platform to perform visualization and analytics which lets you add queries and explore metrics to customize your graphs.
  To create dashboards, you need to add Prometheus metrics as a data source in Grafana. The visualization it provides gives a better understanding of the health and performance of your YugabyteDB clusters. 

  This tutorial uses the [yugabyted](https://docs.yugabyte.com/latest/reference/configuration/yugabyted) local cluster management utility. 
  The purpose of this tutorial is to utilize the Prometheus metrics obtained from following [Prometheus Integration](https://docs.yugabyte.com/latest/explore/observability/prometheus-integration/macos/) and then  creating dashboards via Grafana.
  ## Prerequisite

  A local instance of Prometheus displaying the metrics of your YugabyteDB cluster is required. To do this,
  - Create a local instance of Prometheus and perform  the Steps 1-4 listed under [Prometheus Integration](https://docs.yugabyte.com/latest/explore/observability/prometheus-integration/macos/). (Step 5 is optional)

  ## 1. Setup Grafana and add Prometheus as a data source.
  - Install Grafana and start the server from the [Grafana documentation](https://grafana.com/docs/grafana/latest/installation/mac/).
  - Open the Grafana UI on http://localhost:3000. The default login is `admin` / `admin`. 
  - Follow the steps to create a Prometheus data source from this [page](https://prometheus.io/docs/visualization/grafana/). 

  ## 2. Create a dashboard
  There are different ways to create a dashboard in Grafana. For this tutorial , you will need to use the import option and pick the Yugabytedb community dashboard present under the list of Grafana [Dashboards](https://grafana.com/grafana/dashboards/12620).

  - On the Grafana UI, select the `+` icon and choose the import option.

  ![Grafana import](/images/ce/grafana-add.png)

  - On the next page, enter the Yugabytedb dashboard ID in the `import via grafana.com` option and click `Load`.

  ![Grafana import](/images/ce/grafana-import.png)

  - The next screen will require you to provide a name for your dashboard , a unique identifier and the prometheus data source. Make sure to choose the data source you created earlier.

  ![Grafana dashboard](/images/ce/graf-dash-details.png)

  - After clicking import, you can see the new dashboard with various details starting with the status of your Master and Tserver Nodes. The metrics displayed further are categorized by API types (YSQL, YCQL) and by API methods(Insert, Select, Update etc). 

  ![Grafana dashboard](/images/ce/graf-server-status.png)

  - Since this example uses the `CassandraKeyValue` workload generator from the [Prometheus Integration](https://docs.yugabyte.com/latest/explore/observability/prometheus-integration/macos/) page, you can see different YCQL related metrics in addition to the Master and Tserver statuses. 
  Following are some of the YCQL and RocksDB metrics:

  ![Grafana YCQL](/images/ce/graf-ycql-ops.png)

  ![Grafana YCQL](/images/ce/graf-ycql-select.png)

  ![Grafana RocksDB](/images/ce/graf-rocksdb.png)

  ## 3. Clean up (optional)

  Optionally, you can shut down the local cluster created earlier as a prerequisite.

  ```sh
  $ ./bin/yugabyted destroy \
                    --base_dir=node-1
  ```

  ```sh
  $ ./bin/yugabyted destroy \
                    --base_dir=node-2
  ```

  ```sh
  $ ./bin/yugabyted destroy \
                    --base_dir=node-3
  ```