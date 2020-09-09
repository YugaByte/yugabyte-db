---
title: Security checklist
headerTitle: Security checklist
linkTitle: Security checklist
description: Review security measures for your YugabyteDB installation.
block_indexing: true
menu:
  v2.1:
    identifier: security-checklist
    parent: secure
    weight: 710
isTocNested: true
showAsideToc: true
---

Below are a list of security measures that can be implemented to protect your YugabyteDB installation.

## Enable authentication

Authentication requires that all clients provide valid credentials before they can connect to a YugabyteDB cluster. The authentication credentials in YugabyteDB are stored internally in the YB-Master system tables. The authentication mechanisms available to users depends on what is supported and exposed by the YSQL, YCQL, and YEDIS APIs.

Read more about [how to enable authentication in YugabyteDB](../authentication).

## Configure role-based access control

Roles can be modified to grant users or applications only the essential privileges based on the operations they need to perform against the database. Typically, an administrator role is created first. The administrator then creates additional roles for users.

See the [authorization](../authorization) section to enable role-based access control in YugabyteDB.

## Run as a dedicated user

Run the YB-Master and YB-TServer services with a dedicated operating system user account. Ensure that this dedicated user account has permissions to access the data drives, but no unnecessary permissions.

## Limit network exposure

### Restrict machine and port access

Ensure that YugabyteDB runs in a trusted network environment.  Here are some steps to ensure that:

* Servers running YugabyteDB services are directly accessible only by the servers running the application and database administrators.

* Only servers running applications can connect to YugabyteDB services on the RPC ports. Access to the [YugabyteDB ports](../../deploy/checklist/#default-ports-reference) should be denied to everybody else.

### RPC bind interfaces

Limit the interfaces on which YugabyteDB instances listen for incoming connections. Specify just the required interfaces when starting `yb-master` and `yb-tserver` by using the `--rpc_bind_addresses` option. Do not bind to the loopback address. Read more in the [Admin Reference](../../reference/configuration/yb-tserver/) section on how to use these options when starting `yb-master` and `yb-tserver` services.

### Tips for public clouds

* Do not assign a public IP address to the nodes running YugabyteDB, if possible. Applications can connect to YugabyteDB over private IP addresses.

* In Amazon Web Services (AWS), run the YugabyteDB cluster in a separate VPC ([Amazon Virtual Private Network](https://docs.aws.amazon.com/vpc/latest/userguide/what-is-amazon-vpc.html)) and peer this only with VPCs from which database access is required, for example from those VPCs where the application will run.

* Make the security groups assigned to the database servers very restrictive. Ensure that they can communicate with each other on the necessary ports, and expose only the client accessible ports to just the required set of servers. See the [list of YugabyteDB ports](../../deploy/checklist/#default-ports-reference).

## Enable encryption in transit

[TLS encryption](https://en.wikipedia.org/wiki/Transport_Layer_Security) ensures that network communication between servers is secure. You can configure YugabyteDB to use TLS to encrypt intra-cluster and client to server network communication. Yugabyte recommends enabling encryption in transit in YugabyteDB clusters and clients to ensure privacy and integrity of data transferred over the network.

Read more about enabling [Encryption in transit](../tls-encryption) in YugabyteDB.

## Enable encryption at rest

[Encryption at rest](https://en.wikipedia.org/wiki/Data_at_rest#Encryption) ensures that data
at rest, stored on disk, is protected. You can configure YugabyteDB with a user generated symmetric key to
perform cluster-wide encryption.
