---
title: HMGET
linkTitle: HMGET
description: HMGET
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2160
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HMGET key field [field ...]`</b><br>
This command fetches one or more values for the given fields of the hash that is associated with the given `key`.

<li>For every given `field`, (null) is returned if either `key` or `field` does not exist.</li>
<li>If `key` is associated with a non-hash data, an error is raised.</li>

## Return value

Returns list of string values of the fields in the same order that was requested.

## Examples

```sh
$ HMSET yugahash area1 "Africa" area2 "America"
```

```
"OK"
```

```sh
$ HMGET yugahash area1 area2 area_none
```

```
1) "Africa"
2) "America"
3) (null)
```

## See also

[`hdel`](../hdel/), [`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
