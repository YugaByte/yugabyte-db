---
title: Deploy YugabyteDB in Google Cloud Platform with GCP Deployment Manager
headerTitle: Google Cloud Platform
linkTitle: Google Cloud Platform
description: Use the GCP Deployment Manager to deploy a YugabyteDB cluster in Google Cloud Platform.
block_indexing: true
menu:
  v2.1:
    identifier: deploy-in-gcp-1-deployment-manager
    parent: public-clouds
    weight: 640
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/deploy/public-clouds/gcp/gcp-deployment-manager" class="nav-link active">
      <i class="icon-shell"></i>
      Google Cloud Deployment Manager
    </a>
  </li>

  <li>
    <a href="/latest/deploy/public-clouds/gcp/gke" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Google Kubernetes Engine (GKE)
    </a>
  </li>

  <li >
    <a href="/latest/deploy/public-clouds/gcp/terraform" class="nav-link">
      <i class="icon-shell"></i>
      Terraform
    </a>
  </li>

</ul>

## Prerequisites

* Download and Install [gcloud](https://cloud.google.com/sdk/docs/) command line tool.
* Clone git repo from [here](https://github.com/yugabyte/gcp-deployment-manager.git)

## Deploying using Cloud Shell

<a href="https://console.cloud.google.com/cloudshell/editor?cloudshell_git_repo=https%3A%2F%2Fgithub.com%2Fyugabyte%2Fgcp-deployment-manager.git" target="_blank">
    <img src="https://gstatic.com/cloudssh/images/open-btn.svg"/>
</a>

* Change current directory to cloned git repo directory
* Use gcloud command to create deployment-manager deployment <br/>

    ```
    $ gcloud deployment-manager deployments create <your-deployment-name> --config=yugabyte-deployment.yaml
    ```

* Wait for 5-10 minutes after the creation of all resources is complete by the above command.
* Once the deployment creation is complete, you can describe it as shown below. <br/>

    ```
    $ gcloud deployment-manager deployments describe <your-deployment-name>
    ```

    In the output, you will get the YugabyteDB admin URL, JDBC URL, YSQL, YCQL and YEDIS connection string. You can use YugabyteDB admin URL to access admin portal.
