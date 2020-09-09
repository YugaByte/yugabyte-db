---
title: INCR
linkTitle: INCR
description: INCR
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2210
aliases:
  - /stable/api/redis/incr
  - /stable/api/yedis/incr
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`INCR key`</b><br>
This command adds 1 to the number that is associated with the given `key`. The numeric value must a 64-bit signed integer.
<li>If the `key` does not exist, the associated string is set to "0".</li>
<li>If the given `key` is associated with a non-string value, or if its associated string cannot be converted to an integer, an error is raised.</li>

## Return value

Returns the value after addition.

## Examples

```sh
$ SET yugakey 7
```

```
"OK"
```

```sh
$ INCR yugakey
```

```
8
```

## See also

[`append`](../append/), [`get`](../get/), [`getrange`](../getrange/), [`getset`](../getset/), [`incrby`](../incrby/), [`set`](../set/), [`setrange`](../setrange/), [`strlen`](../strlen/)
