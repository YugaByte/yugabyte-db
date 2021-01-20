-- Copyright (c) YugaByte, Inc.

alter table task_info alter column task_type TYPE varchar(50);
update task_info set task_type = 'CreateCassandraTable' where task_type = 'CreateCassTable';
alter table task_info add column parent_uuid uuid;
alter table task_info add column position integer default -1;
alter table task_info add column sub_task_group_type varchar(50);
