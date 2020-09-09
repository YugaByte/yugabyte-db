---
title: SREM
linkTitle: SREM
description: SREM
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2310
aliases:
  - /stable/api/redis/srem
  - /stable/api/yedis/srem
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`SREM key value [value ...]`</b><br>
This command removes one or more specified members from the set that is associated with the given `key`.
<li>If the `key` does not exist, the associated set is an empty set, and the return value is zero.</li>
<li>If the `key` is associated with a value that is not a set, an error is raised.</li>
<li>If a specified `value` does not exist in the given set, that `value` is ignored and not counted toward the total of removed members.</li>

## Return value

Returns the total number of existed members that were removed from the set.

## Examples

```sh
$ SADD yuga_world "America"
```

```
1
```

```sh
$ SADD yuga_world "Moon"
```

```
1
```

```sh
$ SREM yuga_world "Moon"
```

```
1
```

```sh
$ SREM yuga_world "Moon"
```

```
0
```

## See also

[`sadd`](../sadd/), [`scard`](../scard/), [`sismember`](../sismember/), [`smembers`](../smembers/)
