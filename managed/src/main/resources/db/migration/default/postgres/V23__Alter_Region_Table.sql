-- Copyright (c) YugaByte, Inc.

-- These should match the metadata in devops: opscli/ybops/data/aws-metadata.yml
-- These should also be in sync with the UI data in YW: ui/src/components/config/PublicCloud/views/AWSProviderInitView.js

-- AWS
update region r set yb_image = 'ami-65e0e305' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-west-1';
update region r set yb_image = 'ami-a042f4d8' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-west-2';
update region r set yb_image = 'ami-4bf3d731' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-east-1';
update region r set yb_image = 'ami-25bd2743' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-northeast-1';
update region r set yb_image = 'ami-d2fa88ae' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-southeast-1';
update region r set yb_image = 'ami-b6bb47d4' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-southeast-2';
update region r set yb_image = 'ami-337be65c' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-central-1';
update region r set yb_image = 'ami-6e28b517' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-west-1';
update region r set yb_image = 'ami-f9adef95' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'sa-east-1';
update region r set yb_image = 'ami-e1496384' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-east-2';
update region r set yb_image = 'ami-ee6a718a' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-west-2';
update region r set yb_image = 'ami-bfff49c2' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-west-3';
update region r set yb_image = 'ami-dcad28b8' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ca-central-1';
update region r set yb_image = 'ami-5d99ce32' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-south-1';

-- GCP
update region r set yb_image = 'https://www.googleapis.com/compute/beta/projects/centos-cloud/global/images/centos-7-v20171213' from provider p where p.uuid = r.provider_uuid and p.code = 'gcp';
