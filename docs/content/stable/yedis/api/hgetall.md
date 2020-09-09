---
title: HGETALL
linkTitle: HGETALL
description: HGETALL
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2130
aliases:
  - /stable/api/redis/hgetall
  - /stable/api/yedis/hgetall
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HGETALL key`</b><br>
This command fetches the full content of all fields and all values of the hash that is associated with the given `key`.

<li>If the given `key` does not exist, and empty list is returned.</li>
<li>If the given `key` is associated with non-hash data, an error is raised.</li>

## Return value

Returns list of fields and values.

## Examples

You can do this as shown below.

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
$ HGETALL yugahash
```

```
1) area1
2) "Africa"
3) area2
4) "America"
```

## See also

[`hdel`](../hdel/), [`hexists`](../hexists/), [`hget`](../hget/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
