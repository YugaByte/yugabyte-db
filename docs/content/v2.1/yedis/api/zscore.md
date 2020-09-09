---
title: ZSCORE
linkTitle: ZSCORE
description: ZSCORE
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2545
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`ZSCORE key member`</b><br>
Returns the score of the member in the sorted set at key. If member does not exist in the sorted set, 
or key does not exist, null is returned. If `key` is associated with non sorted set data, 
an error is returned.

## Return value

The score of member (a double precision floating point number), represented as string.

## Examples

```sh
$ ZADD z_key 1.0 v1
```

```
(integer) 1
```

```sh
$ ZSCORE z_key v1
```

```
"1.0"
```

```sh
$ ZSCORE z_key v2
```

```
(null)
```

## See also

[`zadd`](../zadd/), [`zcard`](../zcard/), [`zrange`](../zrange/), [`zrangebyscore`](../zrangebyscore/), [`zrem`](../zrem/), [`zrevrange`](../zrevrange)
