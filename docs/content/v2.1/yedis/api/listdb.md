---
title: LISTDB
linkTitle: LISTDB
description: LISTDB
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2036
isTocNested: true
showAsideToc: true
---

## Synopsis

`LISTDB` is used to list all the yedis database(s) available for use. All databases other than the default database ("0") need to be created using the `CREATEDB` command before use.

A client can issue the `LISTDB` command through the redis-cli.

## Return value

Returns an array of string values, with the yedis database names. 

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
