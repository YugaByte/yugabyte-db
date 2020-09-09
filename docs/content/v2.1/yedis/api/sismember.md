---
title: SISMEMBER
linkTitle: SISMEMBER
description: SISMEMBER
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2290
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`SISMEMBER key member_value`</b><br>
This command is a predicate for whether or not a value is a member of a set that is associated with the given  `key`.
<li>If the `key` is associated with a value that is not a set, an error is raised.</li>
<li>If the `key` does not exist, its associated set is empty, and the command returns 0.</li>
<li>If the `member` belongs to the given set, an integer of 1 is returned.</li>

## Return Value

Returns 1 if the specified member exists. Returns 0 otherwise.

## Examples

```sh
$ SADD yuga_world "America"
```

```
1
```

```sh
$ SISMEMBER yuga_world "America"
```

```
1
```

```sh
$ SISMEMBER yuga_world "Moon"
```

```
0
```

## See also

[`sadd`](../sadd/), [`scard`](../scard/), [`smembers`](../smembers/), [`srem`](../srem/)
