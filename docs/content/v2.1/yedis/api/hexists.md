---
title: HEXISTS
linkTitle: HEXISTS
description: HEXISTS
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2110
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HEXISTS key field`</b><br>
This is a predicate to check whether or not the given `field` exists in the hash that is specified by the given `key`.
<li>If the given `key` and `field` exist, 1 is returned.</li>
<li>If the given `key` or `field` does not exist, 0 is returned.</li>
<li>If the given `key` is associated with non-hash data, an error is raised.</li>

## Return value

Returns existence status as integer, either 1 or 0.

## Examples

```sh
$ HSET yugahash area1 "America"
```

```
1
```

```sh
$ HEXISTS yugahash area1
```

```
1
```

```sh
$ HEXISTS yugahash area_none
```

```
0
```

## See also

[`hdel`](../hdel/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
