---
title: GETSET
linkTitle: GETSET
description: GETSET
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2090
aliases:
  - /stable/api/redis/getset
  - /stable/api/yedis/getset
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`GETSET key value`</b><br>
This command is an atomic read and write operation that gets the existing value that is associated with the given `key` while rewriting it with the given `value`.

<li>If the given `key` does not exist, the given `value` is inserted for the `key`, and null is returned.</li>
<li>If the given `key` is associated with non-string data, an error is raised.</li>

## Return Value

Returns the old value of the given `key`.

## Examples

```sh
$ SET yugakey 1
```

```
"OK"
```

```sh
$ GETSET yugakey 2
```

```
1
```

## See also

[`append`](../append/), [`get`](../get/), [`getrange`](../getrange/), [`incr`](../incr/), [`incrby`](../incrby/), [`set`](../set/), [`setrange`](../setrange/), [`strlen`](../strlen/)
