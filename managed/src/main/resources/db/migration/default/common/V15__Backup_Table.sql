-- Copyright (c) YugaByte, Inc.

create table backup (
  backup_uuid                 uuid not null,
  customer_uuid               uuid not null,
  backup_info                 json not null,
  state                       varchar(50) not null,
  create_time                 timestamp not null,
  update_time                 timestamp,
  constraint pk_backup primary key (backup_uuid)
);
