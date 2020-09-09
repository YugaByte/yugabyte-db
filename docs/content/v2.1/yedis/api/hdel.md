---
title: HDEL
linkTitle: HDEL
description: HDEL
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2100
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`HDEL key field [field ...]`</b><br>
This command removes the given `fields` from the hash that is associated with the given `key`.

<li>If the given `key` does not exist, it is characterized as an empty hash, and 0 is returned for no elements are removed.</li>
<li>If the given `key` is associated with non-hash data, an error is raised.</li>

## Return value

Depends on the configuration parameter `emulate_redis_responses`.
<li>
If `emulate_redis_responses` is `true`, returns
the number of existing fields in the hash that were removed by this command.
</li>
<li>
If `emulate_redis_responses` is `false`, returns OK.
</li>

## Examples

<li> `emulate_redis_responses` is `true`.

```sh
$ HSET yugahash moon "Moon"
```

```
1
```

```sh
$ HDEL yugahash moon
```

```
1
```

```sh
$ HDEL yugahash moon
```

```
0
```
</li>

<li> `emulate_redis_responses` is `false`.

```sh
$ HSET yugahash moon "Moon"
```

```
"OK"
```

```sh
$ HDEL yugahash moon
```

```
"OK"
```

```sh
$ HDEL yugahash moon
```

```
"OK"
```
</li>

## See also

[`hexists`](../hexists/), [`hget`](../hget/), [`hgetall`](../hgetall/), [`hkeys`](../hkeys/), [`hlen`](../hlen/), [`hmget`](../hmget/), [`hmset`](../hmset/), [`hset`](../hset/), [`hincrby`](../hincrby/), [`hstrlen`](../hstrlen/), [`hvals`](../hvals/)
