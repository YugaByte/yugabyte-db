---
title: ZRANGE
linkTitle: ZRANGE
description: ZRANGE
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2385
aliases:
  - /stable/api/redis/zrange
  - /stable/api/yedis/zrange
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`ZRANGE key start stop [WITHSCORES]`</b><br>
This command returns `members` ordered from lowest to highest score in the specified range at sorted set `key`.
`start` and `stop` represent the low and high index bounds respectively and are zero-indexed. They can also be negative 
numbers indicating offsets from the end of the sorted set, with -1 being the last element of the sorted set, -2 the penultimate element, and so on. 
If `key` does not exist, an empty list is returned. If `key` is associated with non sorted-set data, an error is returned.

## Return Value

Returns a list of members found in the range specified by `start`, `stop`, unless the WITHSCORES option is specified (see below).

## ZRANGE Options

<li> WITHSCORES: Makes the command return both the `member` and its `score`.</li>

## Examples

You can do this as shown below.

```sh
$ ZADD z_key 1.0 v1 2.0 v2 3.0 v3
```

```
(integer) 3
```

```sh
$ ZRANGE z_key 0 2
```

```
1) "v1"
2) "v2"
3) "v3"
```

With negative indices.

```sh
$ ZRANGE z_key -2 -1
```

```
1) "v2"
2) "v3" 
```

Both positive and negative indices.

```sh
$ ZRANGE z_key 1 -1 WITHSCORES
```

```
1) "v2"
2) "2.0"
3) "v3"
4) "3.0"
```

Empty list returned if key doesn't exist.

```sh
$ ZRANGE z_key_no_exist 0 2  WITHSCORES
```

```
(empty list or set)
```

## See also

[`zadd`](../zadd/), [`zcard`](../zcard/), [`zrangebyscore`](../zrangebyscore/), [`zrem`](../zrem),
[`zrevrange`](../zrevrange)
