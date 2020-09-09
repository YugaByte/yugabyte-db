---
title: Best practices
linkTitle: Best practices
description: Best practices
aliases:
  - /stable/deploy/kubernetes/local-ssd/
block_indexing: true
menu:
  stable:
    identifier: best-practices
    parent: deploy-kubernetes
    weight: 626
type: page
isTocNested: true
showAsideToc: true
---

## Local vs remote SSDs 

Kubernetes gives users the option of using remote disks using dynamic provisioning or local storage which has to be pre-provisioned.

Local storage gives great performance, but the data is not replicated, and can be lost if the node fails. This option is ideal for databases, like YugabyteDB, that manage their own replication and can guarantee high availability (HA).

Remote storage has slightly lower performance but the data is resilient to failures. This type of storage is absolutely essential for databases that do not offer HA (for example, traditional relational databases, like PostgreSQL and MySQL).

Below is a table that summarizes the features and when to use local or remote storage.

<table>
  <tr>
    <th></th>
    <th>Local SSD storage</th> 
    <th>Remote SSD storage</th>
  </tr>
  <tr>
    <td>Provision large disk capacity per node</td>
    <td>Depends on cloud-provider</td> 
    <td>Yes</td>
  </tr>
  <tr>
    <td>Ideal deployment strategy with YugabyteDB</td>
    <td>Use for latency sensitive apps <br> Add remote storage to increase capacity / cost-efficient tiering</td>
    <td>Use for large disk capacity per node</td>
  </tr>
  <tr>
    <td>Disk storage resilient to failures</td>
    <td>No</td> 
    <td>Yes</td>
  </tr>
  <tr>
    <td>Performance - latency</td>
    <td>Lower</td> 
    <td>Higher</td>
  </tr>
  <tr>
    <td>Performance - throughput</td>
    <td>Higher</td> 
    <td>Lower</td>
  </tr>
  <tr>
    <td>Typical cost characteristics</td>
    <td>Lower</td> 
    <td>Higher</td>
  </tr>
  <tr>
    <td>Kubernetes provisioning scheme</td>
    <td>Pre-provisioned</td> 
    <td>Dynamic provisioning</td>
  </tr>
</table>

Thus, it is generally preferable to use local storage where possible for higher performance and lower costs. The [GKE section](../gke/statefulset-yaml/) shows how to deploy YugabyteDB on Kubernetes using local SSDs.

