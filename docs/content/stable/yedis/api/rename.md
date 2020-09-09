---
title: RENAME
linkTitle: RENAME
description: RENAME
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2265
aliases:
  - /stable/api/redis/rename
  - /stable/api/yedis/rename
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`RENAME key1 key2`</b><br>
Limited support: RENAME command is useful to rename one key as another key.
This is currently a best-effort mechanism and is intended to only work when there is
no concurrent updates to either the source or the destination keys. The TTL setting
for the key itself is copied over to the destination key. However, for container
types such as the TimeSeries type, the ttl settings for the sub-keys are not copied.

## Return value

Returns status string.

## See also

[`set`](../set/),
[`get`](../get/),
[`hset`](../hset/),
[`hget`](../hget/),
