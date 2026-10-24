---
title: BYOC onboarding
headerTitle: BYOC onboarding
linkTitle: Onboarding
description: The steps to onboard a YugabyteDB Aeon BYOC deployment in your own cloud account.
headcontent: How BYOC is deployed in your cloud account
tags:
  feature: early-access
menu:
  stable_yugabyte-cloud:
    parent: cloud-byoc
    identifier: cloud-byoc-onboarding
    weight: 20
type: docs
---

Yugabyte deploys BYOC infrastructure into a dedicated AWS account, GCP project, or Azure subscription that you provide. You retain ownership and administrative control of the cloud environment, while Yugabyte provisions and manages the BYOC infrastructure using automation.

Onboarding runs in five steps. You define the requirements and grant access, and Yugabyte configures, deploys, and validates the environment.

## Step 1: Day 0 planning

BYOC onboarding begins with defining the requirements for your deployment, including its cloud location, network topology, security configuration, and integrations.

Review the following items with your cloud, network, and security teams. At this stage, these are planning decisions and inputs. You don't need to provision the BYOC infrastructure yourself. Yugabyte uses the agreed requirements to plan and build the deployment.

- [ ] **Cloud service provider account.** Identify the AWS account, GCP project, or Azure subscription where the BYOC infrastructure will be deployed. We recommend using a new, dedicated one, to isolate Yugabyte-managed infrastructure from resources managed by your teams and maintain a clear separation of responsibilities. Provide the AWS account ID, GCP project ID, or Azure tenant and subscription IDs, as applicable.

- [ ] **YugabyteDB Aeon account.** Identify the [YugabyteDB Aeon](https://cloud.yugabyte.com/) account where the BYOC deployment will be registered. You use this account to manage your YugabyteDB clusters. Provide the account email address or account ID.

- [ ] **Control plane regions.** Choose the high-availability configuration and regions for the control plane components that manage your databases. The control plane can be deployed **in-region**, across multiple availability zones in a single region, or **cross-region**, across multiple regions.

- [ ] **Data plane regions.** Identify the cloud regions where you plan to deploy YugabyteDB clusters. You can add regions later as your requirements change.

- [ ] **Network ranges.** Define the CIDR ranges for the control plane and data plane networks. Yugabyte specifies the required prefix size during planning, and you select the ranges from your own address space. For deployments that use network peering, these ranges must not overlap with networks that will be connected to the BYOC environment. For sizing considerations, see [Set the CIDR and size your VPC](../../cloud-basics/cloud-vpcs/cloud-vpc-intro/#set-the-cidr-and-size-your-vpc).

- [ ] **Service quotas.** Confirm that vCPU, disk, and IP address quotas in the target regions can accommodate the deployment. Yugabyte provides the expected resource footprint during planning.

- [ ] **Organization policies.** Review the AWS service control policies, GCP organization policy constraints, or Azure Policy assignments that apply to the account, and confirm they permit the resources that BYOC creates. Policy restrictions can prevent BYOC resources from being created.

- [ ] **Network connectivity.** Confirm that your network policies allow the connectivity required between the BYOC environment and Yugabyte services. Yugabyte provides the required endpoints and ports during planning and configures the firewall rules within the BYOC networks.

- [ ] **DNS.** If your organization uses internal DNS for access to resources in the BYOC environment, describe your DNS requirements and any integration with your existing DNS infrastructure.

- [ ] **Certificate authority.** If you want to use your organization's certificate authority (CA) for node-to-node and client-to-node certificates, describe your CA setup and certificate issuance and rotation requirements. Otherwise, Yugabyte uses self-signed certificates. For supported options, see [Encryption in transit](../../../yugabyte-platform/security/enable-encryption-in-transit/).

- [ ] **Encryption at rest.** If you want to use a customer-managed key (CMK) for database encryption at rest, identify the key management service and the location of the key. For supported key management services, see [Encryption at rest](../../cloud-secure-clusters/managed-ear/).

- [ ] **Audit logs.** If you want database audit logs exported to your environment, identify the destination and provide an overview of your logging infrastructure. For supported targets, see [Export logs](../../cloud-monitor/logging-export/).

- [ ] **Query logs.** If you want database query logs exported to your environment, identify the destination and any integration requirements. For supported targets, see [Export logs](../../cloud-monitor/logging-export/).

- [ ] **Observability.** Yugabyte collects metrics and system logs required to operate and support the BYOC deployment. If you also want these metrics or logs exported to your environment, identify the destination and provide an overview of your observability infrastructure. For supported metrics targets, see [Export metrics](../../cloud-monitor/metrics-export/).

After you complete the planning checklist, share the information with your Yugabyte team. Yugabyte reviews the requirements with you, works with your teams to resolve any open questions, and confirms the deployment configuration and implementation details.

## Step 2: Yugabyte configures your Aeon account

Yugabyte enables BYOC on your YugabyteDB Aeon account and registers the BYOC deployment with it.

Yugabyte then provides the cloud identity used to bootstrap the BYOC environment and perform infrastructure changes. In Step 3, you grant this identity access to the account, project, or subscription that hosts the deployment.

| Cloud | Identity |
| :--- | :--- |
| AWS | IAM principal |
| GCP | Service account |
| Azure | Service principal in Microsoft Entra ID |

## Step 3: Grant access

Before deployment can begin, Yugabyte needs permission to bootstrap your cloud environment and create the resources required for BYOC.

Grant the deployment identity from Step 2 the elevated role shown below for the account, project, or subscription that hosts the deployment.

| Cloud | Role | Scope |
| :--- | :--- | :--- |
| AWS | AdministratorAccess | Account |
| GCP | Owner (`roles/owner`) | Project |
| Azure | Owner | Subscription |

Elevated access is required to bootstrap the BYOC environment. During bootstrap, Yugabyte provisions the required infrastructure and creates dedicated identities for the control plane and data plane. After bootstrap, normal operations use these identities, and elevated permissions on the deployment identity can be revoked. Yugabyte may ask you to temporarily restore them for future infrastructure changes that require elevated privileges, such as adding regions or updating IAM configuration.

This access is limited to the AWS account, GCP project, or Azure subscription that hosts BYOC and does not extend to the rest of your organization. You retain ownership and administrative control of the environment, while Yugabyte manages the BYOC infrastructure within it using automation. To keep the deployment in a supported state, don't modify Yugabyte-managed resources directly unless coordinated with Yugabyte.

If your organization prefers to manage selected infrastructure components through your own infrastructure-as-code processes, this can be supported for components such as VPCs and subnets, firewall rules, DNS zones, private endpoints, backup storage buckets, and service accounts and IAM roles. The responsibility split and implementation details must be agreed with Yugabyte during Day 0 planning.

## Step 4: Yugabyte deploys the infrastructure

Yugabyte bootstraps the account and deploys the BYOC environment end to end. The deployment creates the following:

- Control plane and data plane virtual networks, subnets, and the network connectivity between them
- Firewall rules for control plane high availability, control plane to node and node to node traffic, application access over YSQL and YCQL, and Yugabyte operator access
- Private endpoints in each network for the management and support connections to Yugabyte, and the private DNS zones that resolve the Yugabyte-provided domains to them
- Virtual machines and disks for a [YugabyteDB Anywhere](../../../yugabyte-platform/) high availability pair
- Identities and role bindings for the control plane and data plane
- Object storage buckets for backups, with versioning and a retention policy
- Internal TCP load balancers for your database clusters, and the node template that lets database nodes authenticate to object storage for backups
- YugabyteDB Anywhere configuration: the cloud provider, backup storage configuration, and backup schedules
- The telemetry pipeline that pushes metrics and logs outbound to Yugabyte over the private endpoint
- A privileged access management (PAM) agent, on the control plane hosts and on every database node, through which Yugabyte performs operational access and troubleshooting

Yugabyte then connects the deployment to your YugabyteDB Aeon account.

## Step 5: Deployment ready

After deployment, Yugabyte validates the environment and confirms that the BYOC deployment is connected to your YugabyteDB Aeon account. Once validation is complete, Yugabyte notifies you that the environment is ready for use.

You can then create and manage YugabyteDB clusters through Aeon in any of the data plane networks agreed during Day 0 planning. Yugabyte continues to operate and maintain the underlying BYOC infrastructure as part of the managed service.

To provide application access to your clusters, connect your application networks to the relevant data plane networks using the connectivity approach agreed during Day 0 planning, typically VPC peering.

## Next steps

- [Create a cluster](../../cloud-basics/create-clusters/)
- [Connect to your cluster](../../cloud-connect/)
