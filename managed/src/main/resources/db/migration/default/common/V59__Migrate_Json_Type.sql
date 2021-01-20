-- Copyright (c) YugaByte, Inc.
-- json type wont work with combination of h2 and ebeans.
-- So we to create  JSON_ALIAS which will just map to json type on postgres
-- and TEXT type on H2.
-- Thus this is a no-op schema change for postgres!
-- There are other solutions that will require a non-no-op schema migration
-- or much bigger java code change to how we process json types in entities.
-- GOING FORWARD please use  JSON_ALIAS instead of JSON type in DDL

-- !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
-- !!!!!!!!  GOING FORWARD use  JSON_ALIAS instead of JSON type in DDL !!!!!!!!!!
-- !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

alter table access_key alter column key_info type  JSON_ALIAS;
alter table availability_zone alter column config  type  JSON_ALIAS;
alter table backup alter column backup_info  type  JSON_ALIAS;
alter table certificate_info alter column custom_cert_info  type  JSON_ALIAS;
alter table customer alter column features  type  JSON_ALIAS;
alter table customer_config alter column data  type  JSON_ALIAS;
alter table kms_config alter column auth_config   type  JSON_ALIAS;
alter table metric_config alter column config  type  JSON_ALIAS;
alter table provider alter column config  type  JSON_ALIAS;
alter table region alter column config  type  JSON_ALIAS;
alter table region alter column details  type  JSON_ALIAS;
alter table schedule alter column task_params  type  JSON_ALIAS;
alter table task_info alter column details  type  JSON_ALIAS;
alter table universe alter column config  type  JSON_ALIAS;
