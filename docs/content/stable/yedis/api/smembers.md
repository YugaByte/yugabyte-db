---
title: SMEMBERS
linkTitle: SMEMBERS
description: SMEMBERS
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2300
aliases:
  - /stable/api/redis/smembers
  - /stable/api/yedis/smembers
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`SMEMBERS key`</b><br>
This command selects all members of the set that is associated with the given `key`.
<li>If `key` is associated with a value that is not a set, an error is raised.</li>
<li>If `key` does not exist, no value is returned.</li>

## Return value

Returns all members of the given set.

## Examples

```sh
$ SADD yuga_world "Africa"
```

```
1
```

```sh
$ SADD yuga_world "America"
```

```
1
```

```sh
$ SMEMBERS yuga_world
```

```
1) "Africa"
2) "America"
```

## See also

[`sadd`](../sadd/), [`scard`](../scard/), [`sismember`](../sismember/), [`srem`](../srem/)
