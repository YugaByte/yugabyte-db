-- Copyright (c) YugaByte, Inc.

-- These should match the metadata in devops: opscli/ybops/data/aws-metadata.yml
-- These should also be in sync with the UI data in YW: ui/src/components/config/PublicCloud/views/AWSProviderInitView.js

-- AWS
update region set yb_image = 'ami-65e0e305' where code = 'us-west-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-a042f4d8' where code = 'us-west-2' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-4bf3d731' where code = 'us-east-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-25bd2743' where code = 'ap-northeast-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-d2fa88ae' where code = 'ap-southeast-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-b6bb47d4' where code = 'ap-southeast-2' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-337be65c' where code = 'eu-central-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-6e28b517' where code = 'eu-west-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-f9adef95' where code = 'sa-east-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-e1496384' where code = 'us-east-2' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-ee6a718a' where code = 'eu-west-2' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-bfff49c2' where code = 'eu-west-3' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-dcad28b8' where code = 'ca-central-1' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-5d99ce32' where code = 'ap-south-1' and provider_uuid = (select uuid from provider where code = 'aws');

-- GCP
update region set yb_image = 'https://www.googleapis.com/compute/beta/projects/centos-cloud/global/images/centos-7-v20171213' where provider_uuid = (select uuid from provider where code = 'gcp');
