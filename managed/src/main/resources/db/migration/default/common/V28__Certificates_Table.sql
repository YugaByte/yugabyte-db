-- Copyright (c) YugaByte, Inc.
create table certificate_info (
  uuid                          uuid not null,
  customer_uuid                 uuid not null,
  label                         varchar(255),
  start_date                    timestamp not null,
  expiry_date                   timestamp not null,
  private_key                   TEXT not null,
  certificate                   TEXT not null,
  constraint pk_cert primary key (uuid)
);
