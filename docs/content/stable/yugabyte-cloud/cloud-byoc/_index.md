---
title: Bring Your Own Cloud (BYOC)
headerTitle: Bring Your Own Cloud (BYOC)
linkTitle: Bring Your Own Cloud (BYOC)
description: Run YugabyteDB Aeon clusters in your own cloud account with Bring Your Own Cloud (BYOC), operated by Yugabyte.
headcontent: The managed experience of YugabyteDB Aeon, in your own cloud account
tags:
  feature: early-access
menu:
  stable_yugabyte-cloud:
    parent: yugabytedb-managed
    identifier: cloud-byoc
    weight: 825
type: indexpage
showRightNav: true
---

Bring Your Own Cloud (BYOC) gives you the managed experience of YugabyteDB Aeon while your database runs entirely in your own cloud account. Yugabyte deploys, upgrades, patches, monitors, and supports your clusters, but your data, storage, and encryption keys never leave your cloud. It's a fully managed database, on your infrastructure, under your governance.

## How it works

YugabyteDB Aeon remains hosted by Yugabyte, while [YugabyteDB Anywhere](../../yugabyte-platform/) and your database clusters run in your own cloud account. Yugabyte manages the deployment remotely over a private, outbound-only connection that never opens an inbound path into your environment. Your data never leaves your cloud.

For the full picture, see [Architecture](cloud-byoc-architecture/).

## What you get

- **Your data stays in your cloud.** Database nodes, storage, backups, and encryption keys remain in your own cloud account and region, so you can meet data residency, sovereignty, and compliance requirements without moving data to a vendor.
- **A fully managed database, not another thing to operate.** Yugabyte handles provisioning, upgrades, OS and database patching, monitoring, alerting, and incident response, backed by the same service-level agreement (SLA), support, and on-call process as YugabyteDB Aeon. You get managed economics and an Aeon-grade operational standard without staffing a database operations team.

## How BYOC is different

BYOC combines the **managed experience of YugabyteDB Aeon** with the **data placement of YugabyteDB Anywhere**: your clusters run in your own cloud, but Yugabyte runs them for you.

| | YugabyteDB Aeon | YugabyteDB Aeon BYOC | YugabyteDB Anywhere |
| :--- | :--- | :--- | :--- |
| Console and APIs | Yugabyte-hosted | Yugabyte-hosted | You run YugabyteDB Anywhere |
| Clusters run in | Yugabyte's cloud account | **Your** cloud account | Your infrastructure |
| Supported environments | AWS, Azure, GCP | AWS, Azure, GCP | Any cloud, on-premises, and Kubernetes |
| Data and keys | Yugabyte's account | **Your** account | Your infrastructure |
| Operated by | Yugabyte | **Yugabyte** | You |
| You manage the infrastructure | No | No | Yes |
| Service-level agreement (SLA) | Yes | Yes (same as Aeon) | Self-managed |

The three options suit different needs:

- Choose **YugabyteDB Aeon** for the simplest fully managed service, when your data can reside in Yugabyte's cloud.
- Choose **YugabyteDB Anywhere** when you want to deploy and operate YugabyteDB yourself, in any environment.
- Choose **BYOC** when your data must stay in your own cloud account and you want Yugabyte to run the database for you.

## Who it's for

- Enterprises with data residency, sovereignty, or compliance requirements that prevent moving data into a vendor-managed cloud.
- Teams that want the economics and simplicity of a managed database without giving up control of their cloud environment.
- Organizations with an established cloud account, IAM, and security model that any vendor needs to work within.

## What Yugabyte standardizes, what you control

BYOC is opinionated where it keeps the service reliable and supportable, and flexible where it needs to fit your environment. It supports almost all YugabyteDB Anywhere features and topologies.

| Yugabyte standardizes (so it stays operable) | You control (so it fits your cloud) |
| :--- | :--- |
| The YugabyteDB Anywhere and OS images | Cloud account, region, and network layout |
| Upgrades and patching | Account-level identity and access, and Aeon SSO |
| Observability, alerting, and incident response | Organization policy and egress guardrails |
| Support tooling and access | Audit and log export destinations |
| Infrastructure-as-code delivery (no manual console changes) | Maintenance windows and resource tagging |

## Security and data sovereignty

BYOC is designed to minimize the trust you place in Yugabyte and keep control in your hands:

- **Your data never leaves your perimeter.** The entire data plane, including database nodes, storage, backups, and encryption keys, stays in your cloud account. Yugabyte does not access or process your database content, application data, or query results.
- **Private, outbound-only connectivity.** All management traffic flows over native private networking ([AWS PrivateLink](https://docs.aws.amazon.com/vpc/latest/privatelink/), [GCP Private Service Connect](https://cloud.google.com/vpc/docs/private-service-connect), or [Azure Private Link](https://learn.microsoft.com/en-us/azure/private-link/private-link-overview)), initiated from your environment. Yugabyte opens no inbound path into your cloud, and traffic never crosses the public internet.
- **No standing access.** Yugabyte has no default access to your environment. Operational support uses customer-approved, time-bound, least-privilege sessions that are strongly authenticated, governed by role-based access control, and fully recorded. You can request audit evidence at any time.
- **Only minimized telemetry crosses the boundary.** Metrics and logs are collected by agents in your environment and pushed outbound over TLS. Credentials, secrets, and tokens are masked, and no database content is collected.
- **Certified operations.** Yugabyte's managed environment is covered by SOC 2, PCI DSS, ISO 27001, ISO 9001, and ISO 22301 certifications.

## Supported clouds

BYOC supports Amazon Web Services (AWS), Microsoft Azure, and Google Cloud Platform (GCP).

During Early Access, cloud availability and supported features may vary. Contact [Yugabyte Sales](https://www.yugabyte.com/contact/) for current details.

## Get started

BYOC is available in Early Access. To see a demo or evaluate BYOC for your organization, contact [Yugabyte Sales](https://www.yugabyte.com/contact/).

## Learn more

{{<index/block>}}

  {{<index/item
    title="Architecture"
    body="How BYOC connects your cloud account to Yugabyte using private connectivity, and what data crosses the boundary."
    href="cloud-byoc-architecture/"
    icon="fa-thin fa-sitemap">}}

  {{<index/item
    title="Onboarding"
    body="The onboarding steps, from Day 0 planning through deployment and handover."
    href="cloud-byoc-onboarding/"
    icon="fa-thin fa-cloud-arrow-up">}}

  {{<index/item
    title="Operations"
    body="How the BYOC environment is operated after handover, and what you manage from YugabyteDB Aeon."
    href="cloud-byoc-operations/"
    icon="fa-thin fa-gears">}}

{{</index/block>}}
