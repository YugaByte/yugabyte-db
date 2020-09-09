---
title: EXPIREAT
linkTitle: EXPIREAT
description: EXPIREAT
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2062
aliases:
  - /stable/api/redis/expireat
  - /stable/api/yedis/expireat
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`EXPIREAT key ttl-as-timestamp`</b><br>
EXPIREAT has the same effect as EXPIRE, but instead of specifying the number of seconds representing the TTL (time to live), it takes an absolute Unix timestamp (seconds since January 1, 1970). A timestamp in the past will delete the key immediately.

## Return value

Returns integer reply, specifically 1 if the timeout was set and 0 if key does not exist.

## Examples

```sh
$ SET yugakey "Yugabyte"
```

```
"OK"
```

```sh
$ EXPIREAT yugakey 1293840000
```

```
(integer) 1
```

```sh
$ EXISTS yugakey
```

```
(integer) 0
```

## See also

[`expire`](../expire/), [`ttl`](../ttl/), [`pttl`](../pttl/), [`set`](../set/) 
