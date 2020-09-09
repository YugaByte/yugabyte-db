---
title: HSTRLEN
linkTitle: HSTRLEN
description: HSTRLEN
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2190
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HSTRLEN key field`</b><br>
This command seeks the length of a string value that is associated with the given `field` in a hash table that is associated with the given `key`.
<li>If the `key` or `field` does not exist, 0 is returned.</li>
<li>If the `key` is associated with a non-hash-table value, an error is raised.</li>

## Return value

Returns the length of the specified string.

## Examples

```sh
$ HMSET yugahash L1 America L2 Europe
```

```
"OK"
```

```sh
$ HSTRLEN yugahash L1
```

```
7
```

## See also

[`hdel`](../hdel/), [`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hvals`](../hvals/)
