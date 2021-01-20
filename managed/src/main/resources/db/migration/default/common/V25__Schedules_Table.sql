-- Copyright (c) YugaByte, Inc.

create table schedule (
  schedule_uuid                 uuid not null,
  customer_uuid                 uuid not null,
  failure_count					        integer default 0 not null,
  frequency                     bigint not null,
  task_params                   json not null,
  task_type                     varchar(50) not null,
  status                        varchar(7) not null,
  constraint ck_schedule_status check (status in ('Active','Paused','Stopped')),
  constraint pk_schedule primary key (schedule_uuid)
);

create table schedule_task (
  task_uuid                     uuid not null,
  schedule_uuid                 uuid not null,
  completed_time                timestamp,
  scheduled_time                timestamp,
  constraint pk_schedule_task primary key (task_uuid)
);
