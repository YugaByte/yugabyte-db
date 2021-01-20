---
title: Prepare the Amazon Web Services (AWS) cloud environment
headerTitle: Prepare the Amazon Web Services (AWS) cloud environment
linkTitle: Prepare the environment
description: Prepare the Amazon Web Services (AWS) environment for the Yugabyte Platform.
aliases:
  - /latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/
menu:
  latest:
    identifier: prepare-environment-1-aws
    parent: install-yugabyte-platform
    weight: 55
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/aws" class="nav-link active">
      <i class="fab fa-aws" aria-hidden="true"></i>
      AWS
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/gcp" class="nav-link">
       <i class="fab fa-google" aria-hidden="true"></i>
      GCP
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/azure" class="nav-link">
      <i class="icon-azure" aria-hidden="true"></i>
      &nbsp;&nbsp; Azure
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

  <li>
    <a href="/latest/yugabyte-platform/install-yugabyte-platform/prepare-environment/on-premises" class="nav-link">
      <i class="fas fa-building" aria-hidden="true"></i>
      On-premises
    </a>
  </li>

</ul>

## 1. Create a new security group (optional)

In order to access Yugabyte Platform from outside the AWS environment, you would need to enable access by assigning an appropriate security group to the Yugabyte Platform machine. You will at minimum need to:

- Access the Yugabyte Platform instance over SSH (port `tcp:22`)
- Check, manage, and upgrade Yugabyte Platform (port `tcp:8800`)
- View the Yugabyte Platform console (port `tcp:80`)

To create a security group that enables these, go to **EC2 > Security Groups**, click **Create Security Group** and then add the following values:

- For the name, enter `yugaware-sg` (you can change the name if you want).
- Add a description (for example, `Security group for Yugabyte Platform access`).
- Add the appropriate IP addresses to the **Source IP ranges** field. To allow access from any machine, add `0.0.0.0/0` but note that this is not very secure.
- Add the ports `22`, `8800`, and `80` to the **Port Range** field. The **Protocol** selected must be `TCP`.

You should see something like the screenshot below. Click **Create** next.

![Create security group](/images/ee/aws-setup/yugaware-aws-create-sg.png)

## 2. Create a new IAM role (optional)

In order for Yugabyte Platform to manage YugabyteDB nodes, limited access to your AWS infrastructure is required. To grant the required access, you can provide a set of credentials when configuring the AWS provider, as described in [Configure the AWS cloud provider](../../../configure-yugabyte-platform/set-up-cloud-provider/aws). Alternatively, the EC2 instance where the Yugabyte Platform will be running can be brought up with an IAM role with enough permissions to take all the actions required by Yugabyte Platform. Here is a sample of such a role:

```sh
{
    "Version": "2020-01-17",
    "Statement": [
        {
            "Sid": "VisualEditor0",
            "Effect": "Allow",
            "Action": [
                "ec2:AttachVolume",
                "ec2:AuthorizeSecurityGroupIngress",
                "ec2:ImportVolume",
                "ec2:ModifyVolumeAttribute",
                "ec2:DescribeInstances",
                "ec2:DescribeInstanceAttribute",
                "ec2:CreateKeyPair",
                "ec2:DescribeVolumesModifications",
                "ec2:DeleteVolume",
                "ec2:DescribeVolumeStatus",
                "ec2:StartInstances",
                "ec2:DescribeAvailabilityZones",
                "ec2:CreateSecurityGroup",
                "ec2:DescribeVolumes",
                "ec2:ModifyInstanceAttribute",
                "ec2:DescribeKeyPairs",
                "ec2:DescribeInstanceStatus",
                "ec2:DetachVolume",
                "ec2:ModifyVolume",
                "ec2:TerminateInstances",
                "ec2:AssignIpv6Addresses",
                "ec2:ImportKeyPair",
                "ec2:DescribeTags",
                "ec2:CreateTags",
                "ec2:RunInstances",
                "ec2:AssignPrivateIpAddresses",
                "ec2:StopInstances",
                "ec2:AllocateAddress",
                "ec2:DescribeVolumeAttribute",
                "ec2:DescribeSecurityGroups",
                "ec2:CreateVolume",
                "ec2:EnableVolumeIO",
                "ec2:DescribeImages",
                "ec2:DescribeVpcs",
                "ec2:DeleteSecurityGroup",
                "ec2:DescribeSubnets",
                "ec2:DeleteKeyPair"
            ],
            "Resource": "*"
        }
    ]
}
```

## 3. Provision an instance for Yugabyte Platform

Create an instance to run the Yugabyte Platform server. In order to do so, go to **EC2 > Instances** and click **Launch Instance**. Fill in the following values.

- Change the boot disk image to `Ubuntu 16.04` and continue to the next step.
![Pick OS Image](/images/ee/aws-setup/yugaware-create-instance-os.png)

- Choose `c5.xlarge` (4 vCPUs are recommended for production) as the machine type. Continue to the next step.

- Choose the VPC, subnet and other settings as appropriate. Make sure to enable the **Auto-assign Public IP** setting, otherwise this machine would not be accessible from outside AWS. If you created an IAM role above, or already had one that you would like to use, provide that under **IAM role**. Continue to the next step.

- Increase the root storage volume size to at least `100GiB`. Continue to the next step.

- Add a tag to name the machine. You can set key to `Name` and value to `yugaware-1`. Continue to the next step.

- Select the `yugaware-sg` security group created in the previous step (or the custom name you chose when setting up the security groups). Launch the instance.

- Pick an existing key pair (or create a new one) in order to access the machine. Make sure you have the ssh access key. This is important to enable `ssh` access to this machine. In this example, assume that the key pair is `~/.ssh/yugaware.pem`.

Finally, click **Launch** to launch the Yugabyte Platform server. You should see a machine being created as shown in the image below.

![Pick OS Image](/images/ee/aws-setup/yugaware-machine-creation.png)
