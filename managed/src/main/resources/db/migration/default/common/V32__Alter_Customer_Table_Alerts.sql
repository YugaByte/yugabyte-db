-- Copyright (c) YugaByte, Inc.
create table alert (
  uuid                          uuid not null,
  customer_uuid                 uuid not null,
  create_time                    timestamp not null,
  type                          varchar(255) not null,
  message                       TEXT not null,
  constraint pk_alerts primary key (uuid),
  constraint fk_customer_uuid foreign key (customer_uuid) references customer (uuid) on delete cascade on update cascade
);
