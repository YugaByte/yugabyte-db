---
title: PUBSUB
linkTitle: PUBSUB
description: PUBSUB
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2550
isTocNested: true
showAsideToc: true
---

## Synopsis

PUBSUB command is useful to view the state of the Pub/Sub system in Yugabyte. These commands only provide information regarding the specific YEDIS
server that is queried. 3 subcommands are supported:
<br>
### <b>`PUBSUB channels [pattern]`</b><br>
Lists the currently active channels (optionally matching the given pattern) on the YEDIS server that is queried -- i.e. channels with one or more
subscribers on the node that the client is connected to.
<br>
### <b>`PUBSUB NUBSUB [channel-1 .. channel-n]`</b><br>
Returns the number of subscribers for the specified channels (not counting the clients that are subscribed to patterns) on the YEDIS server that is queried.
<br>
### <b>`PUBSUB NUMPAT`</b><br>
Returns the number of patterns that are subscribed to on the YEDIS server that is queried.

## See also

[`pubsub`](../pubsub/), 
[`publish`](../publish/), 
[`subscribe`](../subscribe/), 
[`unsubscribe`](../unsubscribe/), 
[`psubscribe`](../psubscribe/), 
[`punsubscribe`](../punsubscribe/)
