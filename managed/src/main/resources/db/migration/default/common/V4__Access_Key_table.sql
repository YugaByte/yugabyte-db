-- Copyright (c) YugaByte, Inc.

create table access_key (
    key_code            varchar(100) not null,
    provider_uuid       uuid not null,
    key_info            json not null,
    constraint pk_access_key primary key (key_code,provider_uuid)
);
