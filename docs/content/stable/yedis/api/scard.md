---
title: SCARD
linkTitle: SCARD
description: SCARD
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2260
aliases:
  - /stable/api/redis/scard
  - /stable/api/yedis/scard
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`SCARD key`</b><br>
This command finds the cardinality of the set that is associated with the given `key`. Cardinality is the number of elements in a set.
<li>If the `key` does not exist, 0 is returned.</li>
<li>If the `key` is associated with a non-set value, an error is raised.</li>

## Return value

Returns the cardinality of the set.

## Examples

You can do this as shown below.

```sh
$ SADD yuga_world "America"
```

```
1
```

```sh
$ SADD yuga_world "Asia"
```

```
1
```

```sh
$ SCARD yuga_world
```

```
2
```

## See also

[`sadd`](../sadd/), [`sismember`](../sismember/), [`smembers`](../smembers/), [`srem`](../srem/)
