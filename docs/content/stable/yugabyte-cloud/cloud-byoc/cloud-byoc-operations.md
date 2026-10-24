---
title: BYOC operations
headerTitle: BYOC operations
linkTitle: Operations
description: How operating a BYOC deployment differs from a standard YugabyteDB Aeon deployment.
headcontent: Operational considerations for BYOC
tags:
  feature: early-access
menu:
  stable_yugabyte-cloud:
    parent: cloud-byoc
    identifier: cloud-byoc-operations
    weight: 30
type: docs
---

After handover, you manage your YugabyteDB clusters from the YugabyteDB Aeon console in the same way as other Aeon deployments. Yugabyte continues to operate the BYOC infrastructure in your cloud environment.

For standard database operations, use the regular Aeon documentation:

- [Manage clusters](../../cloud-clusters/), including scaling, maintenance windows, and database upgrades
- [Monitor clusters](../../cloud-monitor/), including metrics, alerts, and Performance Advisor
- [Back up and restore](../../cloud-clusters/backup-clusters/) and [point-in-time recovery](../../cloud-clusters/aeon-pitr/)
- [Secure clusters](../../cloud-secure-clusters/) and [account administration](../../cloud-admin/)

## Infrastructure changes

Yugabyte manages changes to the BYOC infrastructure. For changes such as adding regions or modifying network configuration, contact your Yugabyte team.

Some changes may require temporary restoration of the elevated permissions granted during [onboarding](../cloud-byoc-onboarding/#step-3-grant-access). Don't modify Yugabyte-managed resources directly unless coordinated with Yugabyte.

## Cloud quotas and billing

BYOC resources run in your cloud account, project, or subscription, so they consume your cloud service quotas and appear on your cloud bill.

Make sure sufficient quota is available when scaling clusters, adding regions, or making other changes that increase resource usage.

## Organization policies

Changes to AWS service control policies, GCP organization policies, Azure Policy assignments, or other organizational controls can affect Yugabyte's ability to provision or operate BYOC resources.

Review policy changes that apply to the BYOC environment before applying them.

## Yugabyte access

When operational access to your environment is required, Yugabyte uses privileged access management with authenticated, role-based, time-bound, and recorded sessions.

Support otherwise follows the same processes and channels as YugabyteDB Aeon.

## Connectivity interruptions

If private connectivity between your environment and Yugabyte is interrupted, your databases continue to serve application traffic. Yugabyte may have reduced operational visibility or be unable to perform management operations until connectivity is restored.

For more information, see [Link failure behavior](../cloud-byoc-architecture/#link-failure-behavior).

## Learn more

- [BYOC architecture](../cloud-byoc-architecture/)
- [BYOC onboarding](../cloud-byoc-onboarding/)
