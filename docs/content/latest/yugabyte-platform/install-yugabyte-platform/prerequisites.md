---
title: Prerequisites
headerTitle: Prerequisites
linkTitle: Prerequisites
description: Prerequisites for installing Yugabyte Platform.
menu:
  latest:
    identifier: prerequisites
    parent: install-yugabyte-platform
    weight: 20
isTocNested: true
showAsideToc: true
---

Yugabyte Platform first needs to be installed on a host computer, and then you configure Yugabyte Platform to work in your on-premises private cloud or in a public cloud environment. In a public cloud environment, Yugabyte Platform spawns instances for starting a YugabyteDB universe. In a private cloud environment, you use Yugabyte Platform to add nodes in which you want to be in the YugabyteDB universe. To manage these nodes, Yugabyte Platform requires SSH access to each of the nodes.

## Supported Linux Distributions

You can install Yugabyte Platform on the following Linux distributions:

- Ubuntu 16.04, 18.04, or 20.04 LTS
- Red Hat Enterprise Linux (RHEL) 7
- Oracle Linux 7
- CentOS 7
- Amazon Linux (AMI) 2014.03, 2014.09, 2015.03, 2015.09, 2016.03, 2016.09, 2017.03, 2017.09, 2018.03, or 2.0
- Other [operating systems supported by Replicated](https://www.replicated.com/docs/distributing-an-application/supported-operating-systems/)

## Hardware Requirements

The node running Yugabyte Platform should meet the following requirements:

- 4 cores or more
- 8 GB RAM or more
- 100 GB SSD disk or more
- 64-bit CPU architecture

## Preparing the Host

You prepare the host as follows:

- For a Docker-based installation, Yugabyte Platform uses [Replicated scheduler](https://www.replicated.com/) for software distribution and container management. You need to ensure that the host can pull containers from the the [Replicated Docker Registries](https://help.replicated.com/docs/native/getting-started/docker-registries/).

  Replicated installs a compatible Docker version if its not pre-installed on the host. The current supported Docker version is 19.03.n.

- For a Kubernetes-based installation, you need to ensure that the host can pull container images from the [Quay.io](https://quay.io/) container registry.

### Airgapped Hosts

Installing Yugabyte Platform on Airgapped hosts, without access to any Internet traffic (inbound or outbound) requires the following:

- Whitelisting endpoints: to install Replicated and Yugabyte Platform on a host with no Internet connectivity, you have to first download the binaries on a computer that has Internet connectivity, and then copy the files over to the appropriate host. In case of restricted connectivity, the following endpoints have to be whitelisted to ensure that they are accessible from the host marked for installation:
  `https://downloads.yugabyte.com`
  `https://download.docker.com`

- Ensuring that Docker Engine version 19.03.n is available. If it is not installed, you need to follow the procedure described in [Installing Docker in airgapped](https://www.replicated.com/docs/kb/supporting-your-customers/installing-docker-in-airgapped/).
- Ensuring that the following ports are open on the Yugabyte Platform host:
  - `8800` – HTTP access to the Replicated UI
  - `80` – HTTP access to the Yugabyte Platform console
  - `22` – SSH
- Ensuring that attached disk storage (such as persistent EBS volumes on AWS) is 100 GB minimum
- Having Yugabyte Platform airgapped install package. Contact Yugabyte Support for more information.
- Signing the Yugabyte Enterprise Platform license agreement. Contact Yugabyte Support for more information.
