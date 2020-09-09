---
title: SUBSCRIBE
linkTitle: SUBSCRIBE
description: SUBSCRIBE
block_indexing: true
menu:
  stable:
    parent: api-yedis
    weight: 2552
aliases:
  - /stable/api/redis/subscribe
  - /stable/api/yedis/subscribe
isTocNested: true
showAsideToc: true
---
`SUBSCRIBE`

## Synopsis

<b>`SUBSCRIBE channel [channel ...]`</b><br>
This command subscribes the client to the specified channel(s). The client will receive a message whenever a
publisher sends a message to any of the channels that it has subscribed to.

## See also

[`pubsub`](../pubsub/), 
[`publish`](../publish/), 
[`subscribe`](../subscribe/), 
[`unsubscribe`](../unsubscribe/), 
[`psubscribe`](../psubscribe/), 
[`punsubscribe`](../punsubscribe/)
