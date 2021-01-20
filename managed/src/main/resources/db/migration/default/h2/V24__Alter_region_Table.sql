-- Copyright (c) YugaByte, Inc.

-- These should match the metadata in devops: opscli/ybops/data/aws-metadata.yml
-- These should also be in sync with the UI data in YW: ui/src/components/config/PublicCloud/views/AWSProviderInitView.js

-- AWS
update region set yb_image = 'ami-b1a59fd1' where code = 'us-west-1' and yb_image = 'ami-65e0e305' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-b63ae0ce' where code = 'us-west-2' and yb_image = 'ami-a042f4d8' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-02e98f78' where code = 'us-east-1' and yb_image = 'ami-4bf3d731' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-4dd5522b' where code = 'ap-northeast-1'and yb_image = 'ami-25bd2743' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-a6e88dda' where code = 'ap-southeast-1'and yb_image = 'ami-d2fa88ae' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-5b778339' where code = 'ap-southeast-2'and yb_image = 'ami-b6bb47d4' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-1e038d71' where code = 'eu-central-1' and yb_image = 'ami-337be65c' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-192a9460' where code = 'eu-west-1' and yb_image = 'ami-6e28b517' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-6b5c1b07' where code = 'sa-east-1' and yb_image = 'ami-f9adef95' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-e0eac385' where code = 'us-east-2' and yb_image = 'ami-e1496384' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-c8d7c9ac' where code = 'eu-west-2' and yb_image = 'ami-ee6a718a' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-0c60d771' where code = 'eu-west-3' and yb_image = 'ami-bfff49c2' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-b111aad5' where code = 'ca-central-1' and yb_image = 'ami-dcad28b8' and provider_uuid = (select uuid from provider where code = 'aws');
update region set yb_image = 'ami-82a3eaed' where code = 'ap-south-1' and yb_image = 'ami-5d99ce32' and provider_uuid = (select uuid from provider where code = 'aws');
