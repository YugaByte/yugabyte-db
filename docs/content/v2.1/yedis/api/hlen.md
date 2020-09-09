---
title: HLEN
linkTitle: HLEN
description: HLEN
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2150
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HLEN key`</b><br>
This command fetches the number of fields in the hash that is associated with the given `key`.
<li>If the `key` does not exist, 0 is returned.</li>
<li>If the `key` is associated with non-hash data, an error is raised.</li>

## Return value

Returns number of fields in the specified hash.

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
$ HLEN yugahash
```

```
2
```

## See also

[`hdel`](../hdel/), [`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
