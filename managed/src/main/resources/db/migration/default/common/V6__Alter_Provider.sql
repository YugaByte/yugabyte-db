-- Copyright (c) YugaByte, Inc.

alter table provider drop constraint uq_provider_code;
alter table provider add column customer_uuid uuid;
alter table provider add column config json;

-- We need to update existing records with some dummy value, before
-- we can make the column not null.
update provider set customer_uuid = '00000000-0000-0000-0000-000000000000'
where customer_uuid is null;

alter table provider alter column customer_uuid set not null;
alter table provider add constraint uq_customer_provider_code unique(customer_uuid, code)
