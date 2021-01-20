-- Copyright (c) YugaByte, Inc.
-- NOTE: This is not a versioned migration. Editing this file will tell flyway to re-apply it!
--      Probably safest to not edit this file
alter table if exists certificate_info add column if not exists checksum varchar(32);
