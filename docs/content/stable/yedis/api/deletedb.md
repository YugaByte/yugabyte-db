---
title: DELETEDB
linkTitle: DELETEDB
description: DELETEDB
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2034
aliases:
  - /stable/api/redis/deletedb
  - /stable/api/yedis/deletedb
isTocNested: true
showAsideToc: true
---

## Synopsis

`DELETEDB` is used to delete a yedis database that is no longer needed.

A client can issue the `DELETEDB` command through the redis-cli.

## Return value

Returns a status string upon success.

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

```sh
$ DELETEDB "3.0"
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

## See also

[`createdb`](../createdb/)
[`listdb`](../listdb/)
[`deletedb`](../deletedb/)
[`flushdb`](../flushdb/)
[`flushall`](../flushall/)
[`select`](../select/)
