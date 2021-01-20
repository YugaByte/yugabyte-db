---
title: Prerequisites
headerTitle: Prerequisites
linkTitle: Prerequisites
description: Prerequisites for installing Yugabyte Platform.
menu:
  stable:
    identifier: prerequisites
    parent: install-yugabyte-platform
    weight: 20
isTocNested: true
showAsideToc: true
---

The Yugabyte Platform first needs to be installed on a host machine. Then you need to configure the Yugabyte Platform to work in your on-premises, private cloud, or public cloud environment. In public clouds, Yugabyte Platform spawns the machines to orchestrate starting the YugabyteDB universe. In private clouds, you need to use the Yugabyte Platform to add nodes that you want to be in a YugabyteDB universe. To manage the nodes, Yugabyte Platform requires SSH access to each of the nodes.

To install Yugabyte Platform, you must meet the following requirements.

## Supported Linux distributions

Yugabyte Platform can be installed on the following Linux distributions:

- Ubuntu: 16.04 or 18.04 LTS.
- Red Hat Enterprise Linux (RHEL): 7 or later.
- CentOS: 7 or later.
- Amazon Linux (AMI): 2014.03, 2014.09, 2015.03, 2015.09, 2016.03, 2016.09, 2017.03, 2017.09, 2018.03, 2.0
- Other [operating systems supported by Replicated](https://www.replicated.com/docs/distributing-an-application/supported-operating-systems/)

## Hardware requirements

The node running Yugabyte Platform should be meet the following requirements:

- 4 cores or more
- 8 GB RAM or more
- 100 GB SSD disk or more
- 64-bit CPU architecture
- Docker Engine installed
  - Recommended version: 17.06.2-ce
  - Supported versions: 1.7.1 to 17.06.2-ce

## Prepare the host

Perform the following steps:

- Install and configure [Docker Engine](https://docs.docker.com/engine/).
- Install and configure [Replicated](https://www.replicated.com/), which is a containerized application itself and needs to pull containers from the [Replicated Docker Registries](https://help.replicated.com/docs/native/getting-started/docker-registries/).
- Verify that the host can pull container images from the [Quay.io](https://quay.io/) container registry.

### Airgapped hosts

To install Yugabyte Platform on Airgapped hosts, without access to any Internet traffic (inbound or outbound), perform the following additional steps:

- Whitelist endpoints
  - In order to install Replicated and the Yugabyte Platform on a host with no Internet connectivity at all, you have to first download the binaries on a machine that has Internet connectivity and then copy the files over to the appropriate host. In case of restricted connectivity, the following endpoints have to be whitelisted to ensure that they are accessible from the host marked for installation.

    ```sh
    https://downloads.yugabyte.com
    https://download.docker.com
    ```

- Docker Engine: supported versions `1.7.1` to `17.03.1-ce`. If not installed, see [Installing Docker in airgapped](https://www.replicated.com/docs/kb/supporting-your-customers/installing-docker-in-airgapped/).
- The following ports should be open on the Yugabyte Platform host:
  - `8800` – HTTP access to the Replicated UI
  - `80` – HTTP access to the Yugabyte Platform console)
  - `22` – SSH
- Attached disk storage (such as persistent EBS volumes on AWS): 100 GB minimum
- The Yugabyte Platform airgapped install package (contact Yugabyte support)
- A Yugabyte Platform license file from [Yugabyte](https://www.yugabyte.com/platform/#request-trial-form)
- Ability to connect from the Yugabyte Platform host to all YugabyteDB data nodes. If this is not set up, set up passwordless SSH.
