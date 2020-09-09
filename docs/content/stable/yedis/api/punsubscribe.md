---
title: PUNSUBSCRIBE
linkTitle: PUNSUBSCRIBE
description: PUNSUBSCRIBE
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2555
aliases:
  - /stable/api/redis/punsubscribe
  - /stable/api/yedis/punsubscribe
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`PUNSUBSCRIBE [pattern [pattern ...]]`</b><br>
This command unsubscribes the client from the specified pattern(s). If no pattern is specified, the client is unsubscribed from all patterns that it has subscribed to.

## See also

[`keys`](../keys/), 
[`pubsub`](../pubsub/), 
[`publish`](../publish/), 
[`subscribe`](../subscribe/), 
[`unsubscribe`](../unsubscribe/), 
[`psubscribe`](../psubscribe/), 
[`punsubscribe`](../punsubscribe/)
