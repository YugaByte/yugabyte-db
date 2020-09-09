---
title: HKEYS
linkTitle: HKEYS
description: HKEYS
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2140
aliases:
  - /stable/api/redis/hkeys
  - /stable/api/yedis/hkeys
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HKEYS key`</b><br>
This command fetches all fields of the hash that is associated with the given `key`.

<li>If the `key` does not exist, an empty list is returned.</li>
<li>If the `key` is associated with non-hash data, an error is raised.</li>

## Return value

Returns list of fields in the specified hash.

## Examples

```sh
$ HSET yugahash area1 "Africa"
```

```
1
```

```sh
$ HSET yugahash area2 "America"
```

```
1
```

```sh
$ HKEYS yugahash
```

```
1) "area1"
2) "area2"
```

## See also

[`hdel`](../hdel/), [`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
