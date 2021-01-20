-- Copyright (c) YugaByte, Inc.

create table audit (
  id                            bigint not null,
  user_uuid                     uuid not null,
  customer_uuid                 uuid not null,
  payload                       TEXT,
  api_call                      TEXT not null,
  api_method                    TEXT not null,
  task_uuid                     uuid,
  timestamp                     timestamp not null,
  constraint uq_audit_task_uuid unique (task_uuid),
  constraint pk_audit primary key (id)
);
create sequence audit_id_seq increment by 1;
