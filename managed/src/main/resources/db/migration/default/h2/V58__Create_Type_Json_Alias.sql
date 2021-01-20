-- Copyright (c) YugaByte, Inc.
-- json type wont work with combination of h2 and ebeans.
-- So we to create  JSON_ALIAS which will just map to json type on postgres
-- and TEXT type on H2.
-- Thus this is a no-op schema change for postgres!
-- There are other solutions that will require a non-no-op schema migration
-- or much bigger java code change to how we process json types in entities.
-- GOING FORWARD please use  JSON_ALIAS instead of JSON type in DDL

-- !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
-- !!!!!!!!  GOING FORWARD use  JSON_ALIAS instead of JSON type in DDL !!!!!!!!!!
-- !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

CREATE DOMAIN "JSON_ALIAS" AS TEXT;
