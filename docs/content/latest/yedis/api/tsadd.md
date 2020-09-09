---
title: TSADD
linkTitle: TSADD
description: TSADD
menu:
  latest:
    parent: api-yedis
    weight: 2400
aliases:
  - /latest/api/redis/tsadd
  - /latest/api/yedis/tsadd
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`TSADD key timestamp value [timestamp value ...] [EXPIRE_IN TTL] [EXPIRE_AT UNIX_TIMESTAMP]`</b><br>
This command sets the data for the given `timestamp` with the given `value` in the time series that
is specified by `key`. This is useful in storing time series like data where the `key` could be a
metric, the `timestamp` is the time when the metric was generated and `value` is the value of the
metric at the given `timestamp`.
<li>If the given `timestamp` already exists in the specified time series, this command overwrites the existing value with the given `value`.</li>
<li>If the given `key` does not exist, a new time series is created for the `key`, and the given values are inserted to the associated given fields.</li>
<li>If the given `key` exists, but is not of time series type, an error is raised.</li>
<li>If the given `timestamp` is not a valid signed 64 bit integer, an error is raised.</li>
<li>`EXPIRE_IN TTL` sets the TTL (time-to-live) in seconds for the entries being added.</li>
<li>`EXPIRE_AT UNIX_TIMESTAMP` ensures that the entries added would expire by the given [`UNIX_TIMESTAMP`](https://en.wikipedia.org/wiki/Unix_time) (seconds since January 1, 1970).</li>

## Return value

Returns the appropriate status string.

## Examples

The timestamp can be arbitrary integers used just for sorting values in a certain order.

```sh
$ TSADD cpu_usage 10 "70"
```

```
"OK"
```

```sh
$ TSADD cpu_usage 20 "80" 30 "60" 40 "90"
```

```
"OK"
```

We could also encode the timestamp as “yyyymmddhhmm”, since this would still produce integers that are sortable by the actual timestamp.

```sh
$ TSADD cpu_usage 201710311100 "50"
```

```
"OK"
```
A more common option would be to specify the timestamp as the unix timestamp

```sh
$ TSADD cpu_usage 1509474505 "75"
```

```
"OK"
```

```sh
$ TSGET cpu_usage 10
```

```
"70"
```

```sh
$ TSGET cpu_usage 201710311100
```

```
"50"
```

```sh
$ TSGET cpu_usage 1509474505
```

```
"75"
```
Set a TTL of 3600 seconds (1 hour) for an entry that you add.

```sh
$ TSADD cpu_usage 60 "70" EXPIRE_IN 3600
```

```
"OK"
```
Ensure that the entry we're adding would expire at the unix_timestamp 3513642307.

```sh
$ TSADD cpu_usage 70 "80" EXPIRE_AT 3513642307
```

```
"OK"
```

## See also
[`tsrem`](../tsrem/), [`tsget`](../tsget/), [`tsrangebytime`](../tsrangebytime/),
[`tsrevrangebytime`](../tsrevrangebytime/), [`tslastn`](../tslastn/), [`tscard`](../tscard/)
