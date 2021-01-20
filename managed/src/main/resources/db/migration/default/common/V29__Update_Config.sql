-- Copyright (c) YugaByte, Inc.

alter table region add column config json;
alter table availability_zone add column config json;
