---
title: TSREM
linkTitle: TSREM
description: TSREM
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2450
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`TSREM key timestamp [timestamp ...]`</b><br>
This command removes one or more specified timestamps from the time series that is associated with the given `key`.
<li>If the `key` exists, but is not of time series type, an error is raised.</li>
<li>If the given `timestamp` is not a valid signed 64 bit integer, an error is raised.</li>
<li>If the provided timestamps don't exist, TSREM still returns "OK". As a result, TSREM just
ensures the provided timestamps no longer exist, but doesn't provide any information about whether
they existed before the command was run.</li>

## Return value

Returns the appropriate status string.

## Examples

The timestamp can be arbitrary integers used just for sorting values in a certain order.

```sh
TSADD ts_key 1 one 2 two 3 three 4 four 5 five 6 six
```

```
"OK"
```

```sh
TSGET ts_key 2
```

```
"two"
```

```sh
TSGET ts_key 3
```

```
"three"
```

```sh
TSRANGEBYTIME ts_key 1 4
```

```
1) "1"
2) "one"
3) "2"
4) "two"
5) "3"
6) "three"
7) "4"
8) "four"
```

```sh
TSREM ts_key 2 3
```

```
"OK"
```

```sh
TSRANGEBYTIME ts_key 1 4
```

```
1) "1"
2) "one"
3) "4"
4) "four"
```

```sh
TSGET ts_key 2
```

```
(nil)
```

```sh
TSGET ts_key 3
```

```
(nil)
```

## See also

[`tsadd`](../tsadd/), [`tsget`](../tsget/), [`tsrangebytime`](../tsrangebytime/),
[`tsrangebytime`](../tsrangebytime/), [`tslastn`](../tslastn/), [`tscard`](../tscard/)
