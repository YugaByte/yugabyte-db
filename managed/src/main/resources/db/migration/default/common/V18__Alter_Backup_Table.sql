-- Copyright (c) YugaByte, Inc.

alter table backup add column task_uuid uuid;
create unique index ix_backup_task_uuid on backup (task_uuid);
