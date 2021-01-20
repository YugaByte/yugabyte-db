alter table universe add column config json;
alter table schedule add column cron_expression varchar(255);