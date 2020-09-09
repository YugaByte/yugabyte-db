---
title: do_clean_start.sql
linkTitle: do_clean_start.sql
headerTitle: do_clean_start.sql
description: Drops and re-creates the dp_results and int_results tables.
block_indexing: true
menu:
  v2.1:
    identifier: do-clean-start
    parent: analyzing-a-normal-distribution
    weight: 20
isTocNested: true
showAsideToc: true
---
Save this script as `do_clean_start.sql`.
```postgresql
-- Get a clean start.
-- These tables will store some query results so that, with
-- all these in a single table, it is easy to use
-- SQL to compare results from different tests.

set client_min_messages = warning;
drop table if exists dp_results cascade;
create table dp_results(
  method  text              not null,
  bucket  int               not null,
  n       int               not null,
  min_s   double precision  not null,
  max_s   double precision  not null,

  constraint dp_results_pk primary key(method, bucket));

drop table if exists int_results cascade;
create table int_results(
  method  text              not null,
  bucket  int               not null,
  n       int               not null,
  min_s   double precision  not null,
  max_s   double precision  not null,

  constraint int_results_pk primary key(method, bucket));
```
