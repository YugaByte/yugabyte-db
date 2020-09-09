---
title: Deploy on Amazon Web Services using AWS CloudFormation
headerTitle: Amazon Web Services
linkTitle: Amazon Web Services
description: Deploy a YugabyteDB cluster on Amazon Web Services using AWS CloudFormation
aliases:
  - /stable/deploy/public-clouds/aws/
block_indexing: true
menu:
  stable:
    identifier: deploy-in-aws-1-cloudformation
    parent: public-clouds
    weight: 630
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/stable/deploy/public-clouds/aws/cloudformation" class="nav-link active">
      <i class="icon-shell"></i>
      CloudFormation
    </a>
  </li>
  <li >
    <a href="/stable/deploy/public-clouds/aws/terraform" class="nav-link">
      <i class="icon-shell"></i>
      Terraform
    </a>
  </li>
  <li>
    <a href="/stable/deploy/public-clouds/aws/manual-deployment" class="nav-link">
      <i class="icon-shell"></i>
      Manual deployment
    </a>
  </li>
</ul>

## Prerequisites

1. You need to have an IAM user who has `AWSCloudFormationFullAccess`, `AmazonEC2FullAccess` and `ssm:GetParameters` privilege.
2. If you are going to create the stack via AWS Console then make sure the user also has `s3:CreateBucket`, `s3:PutObject` and `s3:GetObject` policies.
3. Create an SSH key pair that you want to attach to the nodes.
4. In the region you want to bring up the stack, make sure you can launch new VPCs.  
5. Download the template file.

```sh
$ wget https://raw.githubusercontent.com/yugabyte/aws-cloudformation/master/yugabyte_cloudformation.yaml
```

{{< note title="Note" >}}

When using an instance with local disks (not EBS), the `.yaml` file needs to be changed for YugabyteDB to recognize the local disks.
Here is an example using [i3 instance types](https://github.com/yugabyte/aws-cloudformation/blob/master/yugabyte_cloudformation_i3_example.yaml) 
that formats and mounts the nvme ssd automatically for each host and installs YugabyteDB on that mount.

{{< /note >}}

## AWS Command Line

Create CloudFormation template:

```sh
$ aws cloudformation create-stack \
  --stack-name <stack-name> \
  --template-body file://yugabyte_cloudformation.yaml \
  --parameters  ParameterKey=DBVersion,ParameterValue=2.1.6.0 ParameterKey=KeyName,ParameterValue=<ssh-key-name> \
  --region <aws-region>
```

Once resource creation completes, check that stack was created:

```sh
$ aws cloudformation describe-stacks \
  --stack-name <stack-name> \
  --region <aws-region>
```

From this output, you will be able to get the VPC id and YugabyteDB admin URL.

Because the stack creates a security group that restricts access to the database, you might need to update the security group inbound rules if you have trouble connecting to it. 
if you have trouble connecting to the DB.

## AWS Console

1. Navigate to your AWS console and open the CloudFormation dashboard. Click **Create Stack**.

<img title="Cloud Formation dashboard" class="expandable-image" src="/images/deploy/aws/aws-cf-initial-dashboard.png" />
<br>

2. Prepare template using the downloaded template

<img title="Prepare template" class="expandable-image" src="/images/deploy/aws/aws-cf-prepare-template.png" />
<br>

3. Specify the template file downloaded in Prerequisites: `yugabyte_cloudformation.yaml`

<img title="Upload template" class="expandable-image" src="/images/deploy/aws/aws-cf-upload-template.png" />
<br>

4. Provide the required parameters. Each of these fields are prefilled with information from the configuration YAML file that was uploaded. `LatestAmiId` refers to the id of the machine image to use, see [more](https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/finding-an-ami.html).

<img title="Provide Parameters" class="expandable-image" src="/images/deploy/aws/aws-cf-provide-parameters.png" />
<br>

5. Configure Stack options. For more information about these options, click [here](https://docs.aws.amazon.com/AWSCloudFormation/latest/UserGuide/cfn-console-add-tags.html).

<img title="Configure options" class="expandable-image" src="/images/deploy/aws/aws-cf-configure-options.png" />
<br>

6. Finalize changes before creation

<img title="Review stack" class="expandable-image" src="/images/deploy/aws/aws-cf-review-stack.png" />
<br>

7. Check stack output in dashboard

<img title="Check output" class="expandable-image" src="/images/deploy/aws/aws-cf-check-output.png" />
