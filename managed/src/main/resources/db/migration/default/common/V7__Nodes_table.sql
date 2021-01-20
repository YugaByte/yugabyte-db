-- Copyright (c) YugaByte, Inc.

create table node_instance (
    node_uuid                uuid not null,
    zone_uuid                uuid not null,
    node_name                varchar(255) not null,
    instance_type_code       varchar(255) not null,
    in_use                   boolean default false not null,
    node_details_json        TEXT not null,
    constraint pk_node_instance primary key (node_uuid)
);
