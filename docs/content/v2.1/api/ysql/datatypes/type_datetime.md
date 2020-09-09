---
title: Date and time data types [YSQL]
headerTitle: Date and time data types
linkTitle: Date and time
description: YSQL supports the DATE, TIME, TIMESTAMP, and INTERVAL data types.
block_indexing: true
menu:
  v2.1:
    identifier: api-ysql-datatypes-datetime
    parent: api-ysql-datatypes
isTocNested: true
showAsideToc: true
---

## Synopsis

The `DATE`, `TIME`, `TIMESTAMP`, and `INTERVAL` data types are supported in YSQL.

Data type | Description | Min | Max |
----------|-------------|-----|-----|
TIMESTAMP [ (p) ] [ WITHOUT TIME ZONE ] | 8-byte date and time | 4713 BC | 294276 AD |
TIMESTAMP [ (p) ] WITH TIME ZONE | 8-byte date and time | 4713 BC | 294276 AD |
DATE | 4-byte date | 4713 BC | 5874897 AD |
TIME [ (p) ] [ WITHOUT TIME ZONE ] | 8-byte time of day | 00:00:00 | 24:00:00 |
TIME [ (p) ] WITH TIME ZONE | 12-byte time of day | 00:00:00+1459 | 24:00:00-1459 |
INTERVAL [ fields ] [ (p) ] | 16-byte time interval | -178000000 years | 178000000 years |

## Description

Date and time inputs can be in various formats, including ISO, SQL, Postgres-extension, and many others.
