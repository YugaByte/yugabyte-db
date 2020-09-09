---
title: Data types [YSQL]
headerTitle: Data types
linkTitle: Data types
description: Data types
summary: YSQL data type overview and specification.
image: /images/section_icons/api/ysql.png
block_indexing: true
menu:
  stable:
    identifier: api-ysql-datatypes
    parent: api-ysql
    weight: 4200
aliases:
  - /stable/api/ysql/datatypes/
isTocNested: true
showAsideToc: true
---

The following table lists the primitive and compound data types in YSQL.

| Data type | Alias | Description |
|-----------|-------|-------------|
| [`bigint`](type_numeric) | [`int8`](type_numeric) | Signed eight-byte integer |
| [`bigserial`](type_serial) | [`serial8`](type_serial) | Autoincrementing eight-byte integer |
| `bit [ (n) ]` <sup>1</sup> | | Fixed-length bit string |
| `bit varying [ (n) ]` <sup>1</sup> | `varbit [ (n) ]` | Variable-length bit string |
| [`boolean`](type_bool) | [`bool`](type_bool) | Logical boolean (true/false) |
| `box` <sup>1</sup> | | Rectangular box |
| [`bytea`](type_binary) | | Binary data |
| [`character [ (n) ]`](type_character) | [`char [ (n) ]`](type_character) | Fixed-length character string |
| [`character varying [ (n) ]`](type_character) | [`varchar [ (n) ]`](type_character) | Variable-length character string |
| `cidr` <sup>1</sup> | | IPv4 or IPv6 network address |
| `circle` <sup>1</sup> | | Circle on a plane |
| [`date`](type_datetime) | | Calendar date (year, month, day) |
| [`double precision`](type_numeric) | [`float8`](type_numeric) | Double precision floating-point number (8 bytes) |
| `inet` <sup>1</sup> | | IPv4 or IPv6 host address |
| [`integer`](type_numeric) | [`int`, `int4`](type_numeric) | Signed four-byte integer |
| [`interval [ fields ] [ (p) ]`](type_datetime) | | Time span |
| [`json`](type_json) <sup>1</sup> | | Textual JSON data |
| [`jsonb`](type_json) <sup>1</sup> | | Binary JSON data, decomposed |
| `line` <sup>1</sup> | | Infinite line on a plane |
| `lseg` <sup>1</sup> | | Line segment on a plane |
| `macaddr` <sup>1</sup> | | Media Access Control (MAC) address |
| `macaddr8` <sup>1</sup> | | Media Access Control (MAC) address (EUI-64 format) |
| [`money`](type_money) | | Currency amount |
| [`numeric [ (p, s) ]`](type_numeric) | [`decimal [ (p, s) ]`](type_numeric) | Exact fixed-point numeric |
| `path` <sup>1</sup> | | Geometric path on a plane |
| `pg_lsn` <sup>1</sup> | | Log Sequence Number |
| `point` <sup>1</sup> | | Geometric point |
| `polygon` <sup>1</sup> | | Closed geometric path |
| [`real`](type_numeric) | [`float4`](type_numeric) | Floating-point number (4 bytes) |
| [`smallint`](type_numeric) | [`int2`](type_numeric) | Signed two-byte integer |
| [`smallserial`](type_serial) | [`serial2`](type_serial) | Autoincrementing two-byte integer |
| [`serial`](type_serial) | [`serial4`](type_serial) | Autoincrementing four-byte integer |
| [`text`](type_character) | | Variable-length character string |
| [`time [ (p) ] [ without time zone ]`](type_datetime) | | Time of day (no time zone) |
| [`time [ (p) ] with time zone`](type_datetime) | [`timetz`](type_datetime) | Time of day, including time zone |
| [`timestamp [ (p) ] [ without time zone ]`](type_datetime) | | Date and time (no time zone) |
| [`timestamp [ (p) ] with time zone`](type_datetime) | [`timestampz`](type_datetime) | Date and time, including time zone |
| `tsquery` <sup>1</sup> | | Text search query |
| `tsvector` <sup>1</sup> | | Text search document |
| `txid_snapshot` <sup>1</sup> | | Transaction ID snapshot |
| [`uuid`](type_uuid) | | Universally unique identifier |
| `xml` <sup>2</sup> | | XML data |

<sup>1</sup> Table columns of this type cannot be part of an `INDEX` `KEY`.

<sup>2</sup> Under development.
