---
title: BYOC architecture
headerTitle: BYOC architecture
linkTitle: Architecture
description: How YugabyteDB Aeon BYOC connects your cloud account to Yugabyte using private connectivity.
headcontent: How BYOC connects your cloud to Yugabyte
tags:
  feature: early-access
menu:
  stable_yugabyte-cloud:
    parent: cloud-byoc
    identifier: cloud-byoc-architecture
    weight: 10
type: docs
---

In a BYOC deployment, YugabyteDB runs entirely inside your cloud account. Yugabyte operates the service remotely over a private, outbound-only connection. This page describes the deployment at a high level. Exact network parameters are provided during onboarding.

## Control plane and data plane

A BYOC deployment has two parts, both running in your cloud account:

- **Control plane.** The [YugabyteDB Anywhere (YBA)](../../../yugabyte-platform/) deployment that provisions and manages your databases. For high availability, it runs as a pair, either across availability zones within one region or across two regions.
- **Data plane.** The YugabyteDB database nodes that serve your application traffic. Your data, storage, backups, and encryption keys live here and never leave your account.

Yugabyte connects to the control plane to operate the service. Database nodes in the data plane send telemetry through the control plane rather than connecting to Yugabyte directly. This hub-and-spoke model keeps the number of private endpoints small and simplifies your DNS and firewall configuration.

## Private connectivity

All traffic between your environment and Yugabyte uses native cloud private networking and never traverses the public internet:

| Cloud | Private connectivity |
| :--- | :--- |
| AWS | [AWS PrivateLink](https://docs.aws.amazon.com/vpc/latest/privatelink/) |
| GCP | [Private Service Connect (PSC)](https://cloud.google.com/vpc/docs/private-service-connect) |
| Azure | [Azure Private Link](https://learn.microsoft.com/en-us/azure/private-link/private-link-overview) |

Yugabyte publishes its management services behind an endpoint service (AWS), service attachment (GCP), or private link service (Azure), one per region. Each control plane network has a matching private endpoint, and a private DNS record that resolves the Yugabyte-provided domain to it. Yugabyte creates these resources as part of the deployment. For failover, each data plane network connects to every control plane network.

## Trust boundary

BYOC is built so that operating the service does not open an inbound path into your environment:

- **Outbound-initiated connectivity.** Connectivity is established from the BYOC environment to Yugabyte over private endpoints. Yugabyte does not initiate network connections into your environment, and no inbound ports are exposed.
- **Network admission control.** Only cloud accounts explicitly authorized by Yugabyte can establish the private connection. Authorization is by AWS account ID, GCP project ID, or Azure subscription and tenant ID.
- **Encryption in transit.** These connections use server-side TLS with Yugabyte-managed certificates, and rotation requires no action from you. Certificates for node-to-node and client-to-node encryption are separate, and can use your own certificate authority.
- **Managed support access.** Support connectivity uses an outbound tunnel, initiated by a privileged access management agent running on the control plane hosts and the database nodes. It does not create a general administrative entry point into your environment, and your databases keep running if it is unavailable.

## Link failure behavior

If the private connection is interrupted, your YugabyteDB databases continue to serve application traffic, and database operations are not affected.

Management operations that rely on this connection are temporarily unavailable. You cannot create, scale, or otherwise modify clusters from the YugabyteDB Aeon console until connectivity is restored. Yugabyte also has reduced operational visibility during the interruption, which may result in gaps in metrics and logs, and support access is unavailable. Normal management operations and visibility resume when the connection is restored.

## What crosses the boundary

Over the private connection, the following operational data flows _from_ your environment to Yugabyte:

- System metrics (CPU, memory, disk, and network) and database performance metrics
- Time-series labels and identifiers, including database, table, node, user, and instance names
- System and database process logs, and platform metadata such as version and configuration information
- Query metadata for performance analysis, with literal values normalized or removed
- On-demand diagnostic bundles for support

Yugabyte does not collect database content, application data, query results, or replication traffic. Secrets, encryption keys, passwords, and API tokens are redacted at the point of collection, before anything is transmitted.

Data is transmitted over TLS 1.2 or higher and encrypted at rest in Yugabyte-managed systems, where it is accessible only to authorized personnel with a legitimate business need.

## Responsibilities

BYOC separates ownership of the cloud environment from operation of the managed service.

- **You own the cloud environment.** Resources deployed for BYOC reside in your cloud account, appear on your cloud bill, and are subject to your organization's policies and audit controls. You retain administrative control of the account and can revoke Yugabyte access, although doing so may affect Yugabyte's ability to operate and support the deployment.
- **Yugabyte operates the BYOC infrastructure.** Yugabyte provisions, configures, patches, upgrades, and monitors the BYOC resources in your environment. To keep the deployment in a supported state, don't modify Yugabyte-managed resources unless coordinated with Yugabyte. Components that your teams manage by agreement are the exception.
- **Yugabyte manages the service components.** Yugabyte manages the platform software and operational agents running in your environment, the Yugabyte-side connectivity and management services, and the certificates used for private service connectivity, including certificate rotation.

## Next steps

Review [BYOC onboarding](../cloud-byoc-onboarding/), which covers the onboarding steps from Day 0 planning through deployment and handover.
