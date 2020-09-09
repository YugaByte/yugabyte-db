---
title: Deploy read replica clusters
headerTitle: Read replica clusters
linkTitle: Read replica clusters
description: Deploy read replica clusters to asynchronously replicate data from the primary cluster and guarantee timeline consistency.
block_indexing: true
menu:
  stable:
    parent: multi-dc
    identifier: read-replica-clusters
    weight: 634
type: page
isTocNested: true
showAsideToc: true
---

In a YugabyteDB deployment, replication of data between nodes of your primary cluster runs synchronously and guarantees strong consistency. Optionally, you can create a read replica cluster that asynchronously replicates data from the primary cluster and guarantees timeline consistency (with bounded staleness). A synchronously replicated primary cluster can accept writes to the system. Using a read replica cluster allows applications to serve low latency reads in remote regions.

In a read replica cluster, read replicas are _observer nodes_ that do not participate in writes, but get a timeline-consistent copy of the data through asynchronous replication from the primary cluster.

Use the steps below to deploy a read replica cluster using YugabyteDB. For information on deploying read replica clusters using the Yugabyte Platform, see [Read replicas](../../../yugabyte-platform/manage/read-replicas/).

## Deploy a read replica cluster

Follow the steps here to deploy a read replica cluster that will asynchronously replicate data with a primary cluster.

1. **Start the primary `yb-master` services** and let them form a quorum.
2. **Define the primary cluster placement** using the [`yb-admin modify_placement_info`](../../../admin/yb-admin/#modify-placement-info) command.

    ```sh
    $ ./bin/yb-admin modify_placement_info <placement_info> <replication_factor> [placement_uuid]
    ```

    - *placement_info*: Comma-separated list of availability zones using the format `<cloud1.region1.zone1>,<cloud2.region2.zone2>, ...`.
    - *replication_factor*: Replication factor (RF) of the primary cluster.
    - *placement_uuid*: The placement identifier for the primary cluster, using a meaningful string.

3. **Define the read replica placement** using the [`yb-admin add_read_replica_placement_info`](../../../admin/yb-admin/#add-read-replica-placement-info) command.

    ```sh
    $ ./bin/yb-admin add_read_replica_placement_info <placement_info> <replication_factor> [placement_uuid]
    ```

    - *placement_info*: Comma-separated list of availability zones, using the format `<cloud1.region1.zone1>:<num_replicas_in_zone1>,<cloud2.region2.zone2>:<num_replicas_in_zone_2>,..` These read replica availability zones must be uniquely different than the primary availability zones defined in step 2. If you want to use the same cloud, region, and availability zone as a primary cluster, one option is to suffix the zone with `_rr` (for read replica): for example, `c1.r1.z1` vs `c1.r1.z1_rr`).
    - *replication_factor*: The total number of read replicas.
    - *placement_uuid*: The identifier for the read replica cluster, using a meaningful string.

4. Start the primary `yb-tserver` services, including the following configuration flags:

   - [--placement_cloud *placement_cloud*](../../../reference/configuration/yb-tserver/#placement-cloud)
   - [--placement_region *placement_region*](../../../reference/configuration/yb-tserver/#placement-region)
   - [--placement_zone *placement_zone*](../../../reference/configuration/yb-tserver/#placement-zone)
   - [--placement_uuid *live_id*](../../../reference/configuration/yb-tserver/#placement-uuid)

    **Note:** The placements should match the information in step 2. You do not need to add these configuration flags to your `yb-master` configurations.

5. Start the read replica `yb-tserver` services, including the following configuration flags:

   - [--placement_cloud *placement_cloud*](../../../reference/configuration/yb-tserver/#placement-cloud)
   - [--placement_region *placement_region*](../../../reference/configuration/yb-tserver/#placement-region)
   - [--placement_zone *placement_zone*](../../../reference/configuration/yb-tserver/#placement-zone)
   - [--placement_uuid *read_replica_id*](../../../reference/configuration/yb-tserver/#placement-uuid)

    **Note:** The placements should match the information in step 3.

The primary cluster should begin asynchronous replication with the read replica cluster.
