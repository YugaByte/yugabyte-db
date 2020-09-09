---
title: ZREVRANGE
linkTitle: ZREVRANGE
description: ZREVRANGE
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2540
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`ZREVRANGE key start stop [WITHSCORES]`</b><br>
This command returns `members` ordered from highest to lowest score in the specified range at sorted set `key`.
`start` and `stop` represent the high and low index bounds respectively and are zero-indexed. They can also be negative
numbers indicating offsets from the beginning of the sorted set, with -1 being the first element of the sorted set, -2 the second element and so on.
If `key` does not exist, an empty list is returned. If `key` is associated with non sorted-set data, an error is returned.

## Return value

Returns a list of members found in the range specified by `start`, `stop`, unless the WITHSCORES option is specified (see below).

## ZREVRANGE Options

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
$ ZREVRANGE z_key 0 2
```

```
1) "v3"
2) "v2"
3) "v1"
```

With negative indices.

```sh
$ ZREVRANGE z_key -2 -1
```

```
1) "v2"
2) "v1"
```

Both positive and negative indices.

```sh
$ ZREVRANGE z_key 1 -1 WITHSCORES
```

```
1) "v2"
2) "2.0"
3) "v1"
4) "1.0"
```

(0 and (2 are exclusive bounds.

```sh
$ ZREVRANGE z_key (0 (2
```

```
(empty list or set)
```

Empty list returned if key doesn't exist.

```sh
$ ZREVRANGE z_key_no_exist 0 2  WITHSCORES
```

```
(empty list or set)
```

## See also

[`zadd`](../zadd/), [`zcard`](../zcard/), [`zrange`](../zrange/), [`zrangebyscore`](../zrangebyscore/), [`zrem`](../zrem)
