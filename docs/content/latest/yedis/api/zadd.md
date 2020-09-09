---
title: ZADD
linkTitle: ZADD
description: ZADD
menu:
  latest:
    parent: api-yedis
    weight: 2500
aliases:
  - /latest/api/redis/zadd
  - /latest/api/yedis/zadd
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`ZADD key [NX|XX] [CH] [INCR] score member [score member ...]`</b><br>
This command sets all specified `members` with their respective `scores` in the sorted set
specified by `key`, with multiple `score` `member` pairs possible. If a specified `member` is already in
the sorted set, this command updates that `member` with the new `score`. If the `key` does not exist, a new sorted set
is created, with the specified pairs as the only elements in the set. A `score` should be a double,
while a `member` can be any string.

## Return value

The number of new `members` added to the sorted set, unless the `CH` option is specified (see below).

## ZADD options

<li> XX: Only update `members` that already exist. Do not add new `members`.</li>
<li> NX: Do not update `members` that already exist. Only addd new `members`.</li>
<li> CH: Modify the return value from new `members` added to `members` added or updated.</li>
<li> INCR: Increment the specified `member` by `score`. Only one `score` `member` pair can be specified.</li>

## Examples

Add two elements into the sorted set.

```sh
$ ZADD z_key 1.0 v1 2.0 v2
```

```
(integer) 2
```

Update an element in the set. Note the return value is 0, since you do not add a new member.

```sh
$ ZADD z_key 3.0 v1
```

```
(integer) 0
```

Now you add a new member with an existing score. This is fine, since multiple members can have the same score.

```sh
$ ZADD z_key 3.0 v3
```

```
(integer) 1
```

Now, see the members in the sorted set. Note the sort order by score. Since both v1 and v3 have the same score, you use comparison on the members themselves to determine order.

```sh
$ ZRANGEBYSCORE z_key -inf +inf WITHSCORES
```

```
1) "v2"
2) "2.0"
3) "v1"
4) "3.0"
5) "v3"
6) "3.0"
```

With the `XX` option specified, only update exiting members. Here, only v1 is modified.

```sh
$ ZADD z_key XX 1.0 v1 4.0 v4
```

```
(integer) 0
```

With the `NX` option specified, only add new members. Here only v4 is added.

```sh
$ ZADD z_key NX 0.0 v1 4.0 v4
```

```
(integer) 1
```

With the CH option specified, return number of new and updated members.

```sh
$ ZADD z_key CH 0.0 v1 5.0 v5
```

```
(integer) 2
```

With the INCR option specified, increment by score. Score of v5 should now be 6.0.

```sh
$ ZADD z_key INCR 1.0 v5
```


Let us now see the updated sorted set.

```sh
$ ZRANGEBYSCORE z_key -inf +inf
```

```
1) "v1"
2) "0.0"
3) "v2"
4) "2.0"
5) "v3"
6) "3.0"
7) "v4"
8) "4.0"
9) "v5"
10) "6.0"
```

## See also

[`zcard`](../zcard/), [`zrange`](../zrange/), [`zrangebyscore`](../zrangebyscore/), [`zrem`](../zrem/), [`zrevrange`](../zrevrange)
