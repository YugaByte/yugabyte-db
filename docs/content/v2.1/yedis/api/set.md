---
title: SET
linkTitle: SET
description: SET
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2270
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`SET key string_value [EX seconds] [PX milliseconds] [NX|XX]`</b><br>
This command inserts `string_value` to be hold at `key`, where `EX seconds` sets the expire time in `seconds`, `PX milliseconds` sets the expire time in `milliseconds`, `NX` sets the key only if it does not already exist, and `XX` sets the key only if it already exists.

<li>If the `key` is already associated with a value, it is overwritten regardless of its type.</li>
<li>All parameters that are associated with the `key`, such as data type and time to live, are discarded.</li>

## Return Value

Returns status string.

## Examples

```sh
$ SET yugakey "Yugabyte"
```

```
"OK"
```

```sh
$ GET yugakey
```

```
"Yugabyte"
```

## See also

[`append`](../append/), [`get`](../get/), [`getrange`](../getrange/), [`getset`](../getset/), [`incr`](../incr/), [`incrby`](../incrby/), [`setrange`](../setrange/), [`strlen`](../strlen/)
