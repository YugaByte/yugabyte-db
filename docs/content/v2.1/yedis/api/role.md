---
title: ROLE
linkTitle: ROLE
description: ROLE
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2240
isTocNested: true
showAsideToc: true
---
YEDIS only has `master` role as far as Redis compatibility is concerned.

## Synopsis

<b>`ROLE`</b><br>
This command provides information of a Redis instance, such as its role, its state of replication, its followers, or its master. Roles are either "master", "follower", or "sentinel".
<li>Information of a master instance may include the following.
  <ol>
  <li>"master"</li>
  <li>An integer that represents state of replication</li>
  <li>An array of connected followers { IP address, IP port, State of replication }</li>
  </ol>
</li>

<li>Information of a follower instance may include the following.
  <ol>
  <li>"follower"</li>
  <li>Master IP address</li>
  <li>Master IP port</li>
  <li>Connection state that is either "disconnected", "connecting", "sync", or "connected"</li>
  <li>An integer that represents state of replication</li>
  </ol>
</li>

<li>Information of a sentinel instance may include the following.
  <ol>
  <li>"sentinel"</li>
  <li>An array of master names.</li>
  </ol>
</li>

## Return value

Returns an array of values.

## Examples

```sh
$ ROLE
```

```
1) "master"
2) 0
3) 1) 1) "127.0.0.1"
      2) "9200"
      3) "0"
   2) 1) "127.0.0.1"
      2) "9201"
      3) "0"
```

## See also

[`auth`](../auth/), [`config`](../config/)
