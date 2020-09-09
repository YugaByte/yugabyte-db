---
title: Configure Amazon Web Services (AWS) for YugabyteDB deployments
headerTitle: Configure cloud providers
linkTitle: 4. Configure cloud providers
description: Configure Amazon Web Services (AWS) for YugabyteDB deployments using the YugabyteDB Admin Console
block_indexing: true
menu:
  v2.1:
    identifier: configure-cloud-providers-1-aws
    parent: deploy-enterprise-edition
    weight: 680
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/deploy/enterprise-edition/configure-cloud-providers/aws" class="nav-link active">
      <i class="fab fa-aws"></i>
      AWS
    </a>
  </li>

  <li >
    <a href="/latest/deploy/enterprise-edition/configure-cloud-providers/gcp" class="nav-link">
      <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li >
    <a href="/latest/deploy/enterprise-edition/configure-cloud-providers/azure" class="nav-link">
      <i class="icon-azure" aria-hidden="true"></i>
      Azure
    </a>
  </li>

  <li >
    <a href="/latest/deploy/enterprise-edition/configure-cloud-providers/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li >
    <a href="/latest/deploy/enterprise-edition/configure-cloud-providers/onprem" class="nav-link">
      <i class="fas fa-building"></i>
      On-premises
    </a>
  </li>

</ul>

This page details how to configure Amazon Web Services (AWS) for YugabyteDB using the YugabyteDB Admin Console. If no cloud providers are configured, the main Dashboard page highlights the need to configure at least one cloud provider.

![Configure Cloud Provider](/images/ee/configure-cloud-provider.png)

## Prerequisites

### Amazon Web Services (AWS)

If you plan to run YugabyteDB nodes on Amazon Web Services (AWS), all you need to provide on YugabyteDB Admin Console is your cloud provider credentials. The Yugabyte Platform will use those credentials to automatically provision and de-provision instances that run YugabyteDB. An 'instance' for YugabyteDB includes a compute instance as well as local or remote disk storage attached to the compute instance.

## Configure AWS

Configuring the Yugabyte Platform to deploy universes in AWS provides several options for you to tweak, depending on your preferences:

![AWS Empty Provider](/images/ee/aws-setup/aws_provider_empty.png)

## Provider name

This is an internal tag used for organizing your providers, so you know where you want to deploy your YugabyteDB universes.

## Credentials

In order to actually deploy YugabyteDB nodes in your AWS account, the Yugabyte Platform will require access to a set of cloud credentials. These can be provided in one of the following ways:

- Directly provide your [AWS Access Key ID and Secret Key](http://docs.aws.amazon.com/general/latest/gr/managing-aws-access-keys.html)
- Attach an [IAM role](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/iam-roles-for-amazon-ec2.html) to the Yugabyte Platform VM in the EC2 tab.

## KeyPairs

In order to be able to provision EC2 instances with YugabyteDB, the Yugabyte Platform will require SSH access to these. To that end, there are two  to choose from:

- Allow Yugabyte Platform to create and manage [KeyPairs](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/ec2-key-pairs.html). In this mode, the Yugabyte Platform will create KeyPairs across all the regions you choose to setup and store the relevant private key part of these locally in order to SSH into future EC2 instances.
- Use your own already existing KeyPairs. For this you will need to provide the name of the KeyPair, as well as the private key content and the corresponding SSH user. **Note that currently, all this info must be the same across all the regions you choose to provision!**

## Enabling Hosted Zones

Integrating with hosted zones can make YugabyteDB universes easily discoverable. The Yugabyte Platform can integrate with Route53 to provide you managed CNAME entries for your YugabyteDB universes, which will be updated as you change the set of nodes, to include all the relevant ones for each of your universes.

## Global deployment

For deployment, the Yugabyte Platform aims to provide you with easy access to the many regions that AWS makes available globally. To that end, it allows you to select which regions you wish to deploy to and supports two different ways of configuring your setup, based on your environment:

### Yugabyte Platform-managed configuration

If you choose to use the Yugabyte Platform to configure, own and, manage a full cross-region deployment of VPCs, it will generate a YugabyteDB-specific VPC in each selected region, then interconnect them, as well as the VPC in which the Yugabyte Platform was deployed, through [VPC Peering](https://docs.aws.amazon.com/vpc/latest/userguide/vpc-peering.html). This mode will also setup all the other relevant sub-components in all regions, such as Subnets, Security Groups, and Routing Table entries. Some notes:

- You can **optionally** provide a custom CIDR block for each regional VPC, else we will choose some sensible defaults internally, aiming to not overlap across regions.
- You can **optionally** provide a custom AMI ID to use in each region, else, we will use a recent [AWS Marketplace CentOS AMI](https://wiki.centos.org/Cloud/AWS).

![New Region Modal](/images/ee/aws-setup/aws_new_region.png)

### Self-managed configuration

If you wish to use your own custom VPCs, this is also supported. This will allow you the most level of customization over your VPC setup:

- You **must** provide a VPC ID to use for each region.
- You **must** provide a Security Group ID to use for each region. This will be attached to all YugabyteDB nodes and must allow traffic from all other YugabyteDB nodes, even across regions, if you deploy across multiple regions.
- You **must** provide the mapping of what Subnet IDs to use for each Availability Zone in which you wish to be able to deploy. This is required to ensure the Yugabyte Platform can deploy nodes in the correct network isolation that you desire in your environment.
- You can **optionally** provide a custom AMI ID to use in each region, else, we will use a recent [AWS Marketplace CentOS AMI](https://wiki.centos.org/Cloud/AWS).

![Custom Region Modal](/images/ee/aws-setup/aws_custom_region.png)

One really important note if you choose to provide your own VPC information: **it is your responsibility to have preconfigured networking connectivity!** In the case of a single region deployment, this might simply be a matter of region or VPC local Security Groups. However, across regions, the setup can get quite complex. We suggest using the VPC Peering feature of AWS, such that you can setup private IP connectivity between nodes across regions:

- VPC Peering Connections must be established in an N x N matrix, such that every VPC in every region you configure must be peered to every other VPC in every other region.
- Routing Table entries in every regional VPC should route traffic to every other VPC CIDR block across the PeeringConnection to that respective VPC. This must match the Subnets that you provided during the configuration step.
- Security Groups in each VPC can be hardened by only opening up the relevant ports to the CIDR blocks of the VPCs from which you are expecting traffic.
- Lastly, if you deploy Yugabyte Platform in a different VPC than the ones in which you intend to deploy YugabyteDB nodes, then its own VPC must also be part of this cross-region VPC mesh, as well as setting up Routing Table entries in the source VPC (Yugabyte Platform) and allowing one further CIDR block (or public IP) ingress rule on the Security Groups for the YugabyteDB nodes (to allow traffic from the Yugabyte Platform or its VPC).

## Final notes

If you use the Yugabyte Platform to manage KeyPairs for you and you deploy multiple Yugabyte Platform instances across your environment, then the AWS Provider name should be unique for each instance of Yugabyte Platform integrating with a given AWS Account.

## Marketplace acceptance

Finally, in case you did not provide your own custom AMI IDs, before we can proceed to creating a universe, let us check that you can actually spin up EC2 instances with our default AMIs. Our reference AMIs come from a [Marketplace CentOS 7 Product](https://aws.amazon.com/marketplace/pp/B00O7WM7QW/). Visit that link while logged into your AWS account and click **Continue to Subscribe**.

If you are not already subscribed and have thus not accepted the _Terms and Conditions_, then you should see something like this:

![Marketplace accept](/images/ee/aws-setup/marketplace-accept.png)

If so, please click the **Accept Terms** button and wait for the page to switch to a successful state. You should see the following once the operation completes, or if you had already previously subscribed and accepted the terms:

![Marketplace success](/images/ee/aws-setup/marketplace-success.png)

Now, you are ready to create a YugabyteDB universe on AWS.

## Next step

You are now ready to create YugabyteDB universes as outlined in the next section.
