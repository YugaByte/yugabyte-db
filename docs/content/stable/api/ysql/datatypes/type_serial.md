---
title: Serial data types [YSQL]
headerTitle: Serial data types
linkTitle: Serial
description: YSQL serial data types include SMALLSERIAL (SMALLINT), SERIAL (INTEGER), and BIGSERIAL (BIGINT).
block_indexing: true
menu:
  stable:
    identifier: api-ysql-datatypes-serial
    parent: api-ysql-datatypes
aliases:
  - /stable/api/ysql/datatypes/type_serial
isTocNested: true
showAsideToc: true
---

## Synopsis

SMALLSERIAL, SERIAL, and BIGSERIAL are short notation for sequences of `SMALLINT`, `INTEGER`, and `BIGINT`, respectively.

## Description

```
type_specification ::= SMALLSERIAL | SERIAL | BIGSERIAL
```

- Columns of serial types are auto-incremented.
- `SERIAL` does not imply that an index is created on the column.
