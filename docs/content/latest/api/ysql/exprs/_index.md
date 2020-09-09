---
title: Functions and operators [YSQL]
headerTitle: Functions and operators
linkTitle: Functions and operators
description: YSQL supports all PostgreSQL-compatible built-in functions and operators.
image: /images/section_icons/api/ysql.png
menu:
  latest:
    identifier: api-ysql-exprs
    parent: api-ysql
    weight: 4300
aliases:
  - /latest/api/ysql/exprs/
isTocNested: true
showAsideToc: true
---

YSQL supports all PostgreSQL-compatible built-in functions and operators. The following are the currently documented ones.

| Statement | Description |
|-----------|-------------|
| [`currval`](func_currval) | Returns the last value returned by `nextval()` for the specified sequence in the current session |
| [`lastval`](func_lastval) | Returns the value returned from the last call to `nextval()` (for any sequence) in the current session|
| [`nextval`](func_nextval) | Returns the next value from the session's sequence cache |
| [`JSON functions and operators`](../datatypes/type_json/functions-operators/) | Detailed list of JSON-specific functions and operators |
| [`Array functions and operators`](../datatypes/type_array/functions-operators/) | Detailed list of array-specific functions and operators |
| [`Window functions`](./window_functions/) | Detailed list of SQL window functions |