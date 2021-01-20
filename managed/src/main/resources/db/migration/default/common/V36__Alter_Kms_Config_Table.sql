-- Copyright (c) YugaByte, Inc.

alter table kms_config add column name varchar(100);

update kms_config set name = CONCAT(key_provider, ' KMS Configuration');
