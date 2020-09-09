---
title: SELECT
linkTitle: "SELECT "
description: SELECT
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2038
aliases:
  - /stable/api/redis/select
  - /stable/api/yedis/select
isTocNested: true
showAsideToc: true
---

## Synopsis

`SELECT` is used to change the yedis database that the client is communicating with. By default, all client connections start off communicating with the default database ("0"). To start using a database, other than the default database ("0"), it needs to be pre-created using the `CREATEDB` command before use.

## Return value

Returns a status string if successful. Returns an error if the database is not already created.

## Examples

```sh
$ SET k1 v1
```

```
"OK"
```

```sh
$ GET k1
```

```
"v1"
```

```sh
$ LISTDB
```

```
1) "0"
```

```sh
$ CREATEDB "second"
```

```
"OK"
```

```sh
$ SELECT "second"
```

```
"OK"
```

```sh
$ SET k1 v2
```

```
"OK"
```

```sh
$ GET k1
```

```
"v2"
```

```sh
$ SELECT 0
```

```
"OK"
```

```sh
$ GET k1
```

```
"v1"
```

## See also

[`createdb`](../createdb/)
[`listdb`](../listdb/)
[`deletedb`](../deletedb/)
[`flushdb`](../flushdb/)
[`flushall`](../flushall/)
[`select`](../select/)
