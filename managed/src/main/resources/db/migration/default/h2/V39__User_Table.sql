-- Copyright (c) YugaByte, Inc.

create table users (
  uuid                          uuid not null,
  customer_uuid                 uuid not null,
  email                         varchar(256) not null,
  password_hash                 varchar(256) not null,
  creation_date                 timestamp not null,
  auth_token                    varchar(255),
  auth_token_issue_date         timestamp,
  api_token                     varchar(255),
  features                      TEXT,
  role                          varchar(8) not null,
  is_primary                    boolean not null,
  constraint ck_user_role check (role in ('ReadOnly','Admin')),
  constraint uq_user_email unique (email),
  constraint pk_user primary key (uuid)
);

INSERT into users (uuid, customer_uuid, email, password_hash, creation_date, auth_token, auth_token_issue_date, api_token, role, is_primary)
  SELECT  hash('SHA256', random()::text, 1000)::uuid, uuid, email, password_hash, creation_date, auth_token, auth_token_issue_date, api_token, 'Admin', 'true'
  FROM customer;

alter table customer drop column email;
alter table customer drop column password_hash;
alter table customer drop column auth_token;
alter table customer drop column auth_token_issue_date;
alter table customer drop column api_token;
