---
title: INET data type [YCQL]
headerTitle: INET type
linkTitle: INET
description: Use the INET data type to specify columns for data of IP addresses.
menu:
  latest:
    parent: api-cassandra
    weight: 1410
aliases:
  - /latest/api/cassandra/type_inet
  - /latest/api/ycql/type_inet
isTocNested: true
showAsideToc: true
---

## Synopsis

Use the `INET` data type to specify columns for data of IP addresses.

## Syntax

```
type_specification ::= INET
```

## Semantics

- Columns of type `INET` can be part of the `PRIMARY KEY`.
- Implicitly, values of type `INET` data types are neither convertible nor comparable to other data types.
- Values of text data types with correct format are convertible to `INET`.
- `INET` value format supports text literals for both IPv4 and IPv6 addresses.

## Examples

```sql
example> CREATE TABLE dns_table(site_name TEXT PRIMARY KEY, ip_address INET);
```

```sql
example> INSERT INTO dns_table(site_name, ip_address) VALUES ('localhost', '127.0.0.1');
```

```sql
example> INSERT INTO dns_table(site_name, ip_address) VALUES ('example.com', '93.184.216.34'); 
```

`INET` type supports both ipv4 and ipv6 addresses.

```sql
example> UPDATE dns_table SET ip_address = '2606:2800:220:1:248:1893:25c8:1946' WHERE site_name = 'example.com'; 
```

```sql
example> SELECT * FROM dns_table;
```

```
 site_name   | ip_address
-------------+------------------------------------
 example.com | 2606:2800:220:1:248:1893:25c8:1946
   localhost |                          127.0.0.1
```

## See also

- [Data types](..#data-types)
