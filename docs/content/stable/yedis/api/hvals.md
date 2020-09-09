---
title: HVALS
linkTitle: HVALS
description: HVALS
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2200
aliases:
  - /stable/api/redis/hvals
  - /stable/api/yedis/hvals
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HVALS key`</b><br>
This command selects all the values in the hash that is associated with the given `key`.

<li>If the `key` does not exist, an empty list is returned.</li>
<li>if the `key` is associated with a non-hash data, an error is raised.</li>

## Return value

Returns list of values in the specified hash.

## Examples

```sh
$ HMSET yugahash area1 "America" area2 "Africa"
```

```
"OK"
```

```sh
$ HVALS yugahash
```

```
1) "America"
2) "Africa"
```

## See also

[`hdel`](../hdel/), [`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/)
