---
title: PTTL
linkTitle: PTTL
description: PTTL
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2235
aliases:
  - /stable/api/redis/pttl
  - /stable/api/yedis/pttl
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`PTTL key`</b><br>
Similar to TTL this command returns the remaining time to live of a key that has a timeout set, with the sole difference that TTL returns the amount of remaining time in seconds while PTTL returns it in milliseconds.

## Return value

Returns TTL in milliseconds, encoded as integer response.

## Examples

```sh
$ SET yugakey "Yugabyte"
```

```
"OK"
```

```sh
$ EXPIRE yugakey 10
```

```
(integer) 1
```

```sh
$ PTTL yugakey
```

```
(integer) 9995
```

## See also

[`ttl`](../ttl/), [`set`](../set/), [`expire`](../expire/), [`expireat`](../expireat/)
