-- Copyright (c) YugaByte, Inc.

create table kms_config (
    config_uuid               uuid not null,
    customer_uuid             uuid not null,
    key_provider              varchar(30) not null,
    auth_config               json not null,
    version                   int not null,
    constraint pk_kms_config primary key (config_uuid)
);

create table kms_history (
    config_uuid               uuid not null references kms_config(config_uuid),
    target_uuid               uuid not null,
    type                      varchar(30) not null,
    key_ref                   varchar not null,
    timestamp                 timestamp not null,
    version                   int not null,
    constraint pk_kms_universe_key_history primary key (config_uuid, target_uuid, type, key_ref)
)
