---
title: CREATEDB
linkTitle: CREATEDB
description: CREATEDB
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2032
aliases:
  - /stable/api/redis/createdb
  - /stable/api/yedis/createdb
isTocNested: true
showAsideToc: true
---

## Synopsis

`CREATEDB` is used to create a new yedis database. All databases other than the default database ("0") need to be created before use.

A client can issue the `CREATEDB` command through the redis-cli.
This is required before issuing a `SELECT` command to start using the database.

## Return value

Returns a status string, if creating the database was successful. Returns an error message upon error.

## Examples

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
$ LISTDB
```

```
1) "0"
2) "second"
```

```sh
$ CREATEDB "3.0"
```

```
"OK"
```

```sh
$ LISTDB
```

```
1) "0"
2) "3.0"
3) "second"
```

## See also

[`createdb`](../createdb/)
[`listdb`](../listdb/)
[`deletedb`](../deletedb/)
[`flushdb`](../flushdb/)
[`flushall`](../flushall/)
[`select`](../select/)
