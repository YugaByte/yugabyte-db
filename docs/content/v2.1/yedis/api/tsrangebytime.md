---
title: TSRANGEBYTIME
linkTitle: TSRANGEBYTIME
description: TSRANGEBYTIME
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2440
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`TSRANGEBYTIME key low_ts high_ts`</b><br>
This command fetches the values for the given `low_ts`, `high_ts` range in the time series that is
specified by the given `key`.

<li>If the given `key` is associated with non-timeseries data, an error is raised.</li>
<li>If the given `low_ts` or `high_ts` are not a valid signed 64 bit integers, an error is raised.</li>
<li>`low_ts` and `high_ts` are inclusive unless they are prefixed with `(`, in that case they are
exclusive.</li>
<li>Special bounds `-inf` and `+inf` are also supported to retrieve an entire range</li>

## Return value

Returns a list of timestamp, value pairs found in the range specified by `low_ts`, `high_ts`.

## Examples

You can do this as shown below.

```sh
TSADD ts 1 one 2 two 3 three 4 four 5 five 6 six
```

```
"OK"
```

```sh
TSRANGEBYTIME ts_key 2 4
```

```
1) "2"
2) "two"
3) "3"
4) "three"
5) "4"
6) "four"
```
2 is exclusive

```sh
TSRANGEBYTIME ts_key (2 4
```

```
1) "3"
2) "three"
3) "4"
4) "four"
```

2 and 4 are exclusive

```sh
TSRANGEBYTIME ts_key (2 (4
```

```
1) "3"
2) "three"
```

```sh
TSRANGEBYTIME ts_key -inf 3
```

```
1) "1"
2) "one"
3) "2"
4) "two"
5) "3"
6) "three"
```

```sh
TSRANGEBYTIME ts_key 2 +inf
```

```
 1) "2"
 2) "two"
 3) "3"
 4) "three"
 5) "4"
 6) "four"
 7) "5"
 8) "five"
 9) "6"
10) "six"
```

## See also

[`tsrevrangebytime`](../tsrevrangebytime/), [`tsadd`](../tsadd/), [`tsget`](../tsget/),
[`tsrem`](../tsrem/), [`tslastn`](../tslastn/), [`tscard`](../tscard/)
