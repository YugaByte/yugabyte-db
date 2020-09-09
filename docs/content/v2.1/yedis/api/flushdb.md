---
title: FLUSHDB
linkTitle: FLUSHDB
description: FLUSHDB
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2065
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`FLUSHDB`</b><br>
This command deletes all keys from a database.

This functionality can be disabled by setting the yb-tserver `--yedis_enable_flush` flag to `false`.

## Return value

Returns status string.

## Examples

```sh
$ SET yuga1 "America"
```

```
"OK"
```

```sh
$ SET yuga2 "Africa"
```

```
"OK"
```

```sh
$ GET yuga1
```

```
"America"
```

```sh
$ GET yuga2
```

```
"Africa"
```

```sh
$ FLUSHDB
```

```
"OK"
```

```sh
$ GET yuga1
```

```
(null)
```

```sh
$ GET yuga2
```

```
(null)
```

## See also

[`del`](../del/), [`flushall`](../flushall/),[`deletedb`](../deletedb/)
