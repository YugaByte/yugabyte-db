---
title: PEXPIRE
linkTitle: PEXPIRE
description: PEXPIRE
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2233
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`PEXPIRE key timeout`</b><br>
This command works exactly like EXPIRE but the time to live of the key is specified in milliseconds instead of seconds.

## Return value

Returns integer reply, specifically 1 if the timeout was set and 0 if key does not exist.

## Examples

```sh
$ SET yugakey "Yugabyte"
```

```
"OK"
```

```sh
$ PEXPIRE yugakey 10000
```

```
(integer) 1
```

```sh
$ PTTL yugakey
```

```
(integer) 9995
```

## See also

[`expire`](../expire/), [`ttl`](../ttl/), [`pttl`](../pttl/), [`set`](../set/)
