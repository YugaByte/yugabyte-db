-- Copyright (c) YugaByte, Inc.

alter table task_info drop constraint ck_task_info_task_type;
alter table task_info drop constraint ck_task_info_task_state;
alter table customer_task drop constraint ck_customer_task_type;
