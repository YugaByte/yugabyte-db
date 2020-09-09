---
title: Reserved names [YSQL]
headerTitle: Reserved names
linkTitle: Reserved names
description: List of YSQL system column and database object names that are reserved for internal usage.
summary: List of reserved names
image: /images/section_icons/api/ysql.png
block_indexing: true
menu:
  stable:
    identifier: api-ysql-reserved-names
    parent: api-ysql
    weight: 4600
aliases:
  - /stable/api/ysql/reserved_names
isTocNested: true
showAsideToc: true
---

YSQL reserves the following names for internal usage. Exception will be raised when these names are used even when they are double-quoted.

| Names | Description |
|-------|-------------|
| `oid` | System column |
| `tableoid` | System column |
| `xmin` | System column |
| `cmin` | System column |
| `xmax` | System column |
| `cmax` | System column |
| `ctid` | System column |
| `ybctid` | Virtual column |
| Prefixed with `pg_` | System database objects |
| Prefixed with `yb_` | System database objects |
