-- Copyright (c) YugaByte, Inc.

create table customer_config (
    config_uuid               uuid not null,
    customer_uuid             uuid not null,
    type                      varchar(30) not null,
    name                      varchar(100) not null,
    data                      json not null,
    constraint pk_customer_config primary key (config_uuid)
);
