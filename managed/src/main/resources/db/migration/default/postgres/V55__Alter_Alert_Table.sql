-- Copyright (c) YugaByte, Inc.
ALTER TABLE alert ADD COLUMN state varchar(64) default 'ACTIVE';
ALTER TABLE alert ADD COLUMN send_email boolean default false;
ALTER TABLE alert ADD COLUMN definition_uuid uuid default null;

create table alert_definition (
  uuid                          uuid not null,
  query                         TEXT not null,
  name                          TEXT not null,
  universe_uuid                 uuid not null,
  is_active                     boolean default false,
  customer_uuid                 uuid not null,
  constraint pk_alert_definition primary key (uuid),
  constraint fk_customer_uuid foreign key (customer_uuid) references customer (uuid),
  constraint fk_universe_uuid foreign key (universe_uuid) references universe (universe_uuid)
);

ALTER TABLE alert add CONSTRAINT fk_alert_definition_uuid FOREIGN KEY (definition_uuid) references alert_definition (uuid);
