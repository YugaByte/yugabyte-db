-- Copyright (c) YugaByte, Inc.

-- These should match the metadata in devops: opscli/ybops/data/aws-metadata.yml
-- These should also be in sync with the UI data in YW: ui/src/components/config/PublicCloud/views/AWSProviderInitView.js

-- AWS
update region r set yb_image = 'ami-b1a59fd1' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-west-1' and yb_image = 'ami-65e0e305';
update region r set yb_image = 'ami-b63ae0ce' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-west-2' and yb_image = 'ami-a042f4d8';
update region r set yb_image = 'ami-02e98f78' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-east-1' and yb_image = 'ami-4bf3d731';
update region r set yb_image = 'ami-4dd5522b' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-northeast-1'and yb_image = 'ami-25bd2743';
update region r set yb_image = 'ami-a6e88dda' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-southeast-1'and yb_image = 'ami-d2fa88ae';
update region r set yb_image = 'ami-5b778339' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-southeast-2'and yb_image = 'ami-b6bb47d4';
update region r set yb_image = 'ami-1e038d71' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-central-1' and yb_image = 'ami-337be65c';
update region r set yb_image = 'ami-192a9460' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-west-1' and yb_image = 'ami-6e28b517';
update region r set yb_image = 'ami-6b5c1b07' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'sa-east-1' and yb_image = 'ami-f9adef95';
update region r set yb_image = 'ami-e0eac385' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'us-east-2' and yb_image = 'ami-e1496384';
update region r set yb_image = 'ami-c8d7c9ac' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-west-2' and yb_image = 'ami-ee6a718a';
update region r set yb_image = 'ami-0c60d771' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'eu-west-3' and yb_image = 'ami-bfff49c2';
update region r set yb_image = 'ami-b111aad5' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ca-central-1' and yb_image = 'ami-dcad28b8';
update region r set yb_image = 'ami-82a3eaed' from provider p where p.uuid = r.provider_uuid and p.code = 'aws' and r.code = 'ap-south-1' and yb_image = 'ami-5d99ce32';
