-- Copyright (c) YugaByte, Inc.

create table metric_config (
    config_key                varchar(100) not null,
    config                    json not null,
    constraint pk_metric_config primary key (config_key)
);
