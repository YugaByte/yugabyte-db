---
title: Create clusters
linkTitle: Create clusters
description: Create clusters
headcontent:
image: /images/section_icons/deploy/enterprise.png
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
block_indexing: true
menu:
  v2.1:
    identifier: create-clusters
    parent: yugabyte-cloud
    weight: 641
isTocNested: true
showAsideToc: true
---

As a fully-managed YugabyteDB-as-a-service, Yugabyte Cloud makes it easy for you to create a YugabyteDB cluster without
having to get deep into details you do not want to focus on.

## Create a cluster

When you register for your Yugabyte Cloud account, you start at the [Free Tier level](../free-tier/) and see the following:

![Build a Free Tier cluster](/images/deploy/yugabyte-cloud/create-free-tier-cluster-new.png)

Follow the steps here to create your first cluster:

1. To create your Free Tier cluster, click **Create Cluster**. The **Cloud Provider & Region** page appears:

    ![Create Free Tier cluster details](/images/deploy/yugabyte-cloud/create-free-tier-cluster-details.png)

    {{< note title="Note" >}}

    For the initial beta release, the options available are limited to:

    - **Cloud Provider:** Google Cloud Platform (GCP) or Amazon Web Services (AWS)
    - **Region:** Oregon (`us-west1`)

    Additional options for cloud provider and region will become available in future beta updates.

    {{< /note >}}

2. Click **Create Cluster**. The **Clusters** page appears with the provisioning of your new cluster in progress. When the **State** changes to `Ready`,
your cluster is ready for your use.

![Free Tier cluster ready](/images/deploy/yugabyte-cloud/free-tier-cluster-ready.png)

## Next steps

With your Free Tier cluster ready for use, you can click **Go to cluster** to begin exploring the information available in the Yugabyte Cloud Console.
You can also [create databases](../create-databases/), [manage database access](../manage-access/), and [connect to your cluster with YugabyteDB CLIs and third party tools](../connect-to-clusters/).
