-- Copyright (c) YugaByte, Inc.

create table price_component (
  provider_code                 varchar(255) not null,
  region_code                   varchar(255) not null,
  component_code                varchar(255) not null,
  price_details_json            TEXT not null,
  constraint pk_price_component primary key (provider_code,region_code,component_code)
);
