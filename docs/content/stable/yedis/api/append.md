---
title: APPEND
linkTitle: APPEND
description: APPEND
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2010
aliases:
  - /stable/api/redis/append
  - /stable/api/yedis/append
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`APPEND key string_value`</b><br>
This command appends a value to the end of the string that is associated with the given `key`.
<li>If the `key` already exists, the given `string_value` is appended to the end of the string value that is associated with the `key`.</li>
<li>If the `key` does not exist, it is created and associated with an empty string.</li>
<li>If the `key` is associated with a non-string value, an error is raised.</li>

## Return value

Returns the length of the resulted string after appending.

## Examples

```sh
$ GET yugakey
```

```
"Yuga"
```

```sh
$ APPEND yugakey "Byte"
```

```
8
```

```sh
$ GET yugakey
```

```
"Yugabyte"
```

## See also

[`get`](../get/), [`getrange`](../getrange/), [`getset`](../getset/), [`incr`](../incr/), [`incrby`](../incrby/), [`set`](../set/), [`setrange`](../setrange/), [`strlen`](../strlen/)
