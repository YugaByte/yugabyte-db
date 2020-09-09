---
title: ZRANGEBYSCORE
linkTitle: ZRANGEBYSCORE
description: ZRANGEBYSCORE
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2520
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count]`</b><br>
This command fetches `members` for which `score` is in the given `min` `max` range. `min` and `max` are doubles.
If `key` does not exist, an empty range is returned. If `key` corresponds to a non
sorted-set, an error is raised. Special bounds `-inf` and `+inf` are also supported to retrieve an entire range.
`min` and `max` are inclusive unless they are prefixed with `(`, in which case they are
exclusive.

## Return value

Returns a list of `members` found in the range specified by `min`, `max`, unless the WITHSCORES option is specified (see below).

## ZRANGEBYSCORE options

<li> WITHSCORES: Makes the command return both the `member` and its `score`.</li>
<li> LIMIT offset count: Get a range of the matching elements starting at `offset` up to `count`
elements, where `offset` is 0 indexed.</li>

## Examples

You can do this as shown below.

```sh
$ ZADD z_key 1.0 v1 2.0 v2
```

```
(integer) 2
```
Retrieve all members.

```sh
$ ZRANGEBYSCORE z_key -inf +inf
```

```
1) "v1"
2) "v2"
```
Retrieve all member score pairs.

```sh
$ ZRANGEBYSCORE z_key -inf +inf WITHSCORES
```

```
1) "v1"
2) "1.0"
3) "v2"
4) "2.0"
```
Bounds are inclusive.

```sh
$ ZRANGEBYSCORE z_key 1.0 2.0
```

```
1) "v1"
2) "v2"
```
Bounds are exclusive.

```sh
$ ZRANGEBYSCORE z_key (1.0 (2.0
```

```
(empty list or set)
```
Enforce a limit.

```sh
ZRANGEBYSCORE z_key -inf +inf LIMIT 1 1
```

```
1) "v2"
```

## See also

[`zadd`](../zadd/), [`zcard`](../zcard/), [`zrange`](../zrange/), [`zrem`](../zrem/), [`zrevrange`](../zrevrange)
