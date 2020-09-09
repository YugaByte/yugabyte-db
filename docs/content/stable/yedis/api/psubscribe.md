---
title: PSUBSCRIBE
linkTitle: PSUBSCRIBE
description: PSUBSCRIBE
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2554
aliases:
  - /stable/api/redis/psubscribe
  - /stable/api/yedis/psubscribe
isTocNested: true
showAsideToc: true
---

## Synopsis

<b>`PSUBSCRIBE pattern [pattern ...]`</b><br>
This command subscribes the client to the specified pattern(s). The client will receive a message whenever a publisher sends a message to a channel that matches any of the patterns that it has subscribed to.

## See also

[`keys`](../keys/), 
[`pubsub`](../pubsub/), 
[`publish`](../publish/), 
[`subscribe`](../subscribe/), 
[`unsubscribe`](../unsubscribe/), 
[`psubscribe`](../psubscribe/), 
[`punsubscribe`](../punsubscribe/)
