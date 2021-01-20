-- Copyright (c) YugaByte, Inc.
alter table customer_task drop constraint ck_customer_task_target_type;

alter table customer_task rename universe_uuid to target_uuid;
