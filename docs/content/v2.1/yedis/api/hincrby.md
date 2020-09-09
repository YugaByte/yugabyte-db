---
title: HINCRBY
linkTitle: HINCRBY
description: HINCRBY
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2135
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HINCRBY key field delta`</b><br>
This command adds `delta` to the number that is associated with the given field `field` for the hash `key`. The numeric value must a 64-bit signed integer.
<li>If the `key` does not exist, a new hash container is created. If the field `field` does not exist in the hash container, the associated string is set to "0".</li>
<li>If the given `key` is not associated with a hash type, or if the string  associated with `field` cannot be converted to an integer, an error is raised.</li>

## Return value

Returns the value after addition.

## Examples

```sh
$ HSET yugahash f1 5
```

```
1
```

```sh
$ HINCRBY yugahash f1 3
```

```
8
```

```sh
$ HINCRBY yugahash non-existent-f2 4
```

```
4
```

```sh
$ HINCRBY non-existent-yugahash f1 3
```

```
3
```

## See also

[`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
