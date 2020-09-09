---
title: MONITOR
linkTitle: MONITOR
description: MONITOR
block_indexing: true
menu:
  v2.1:
    parent: api-yedis
    weight: 2218
isTocNested: true
showAsideToc: true
---
`MONITOR` is a debugging tool to see all requests that are being processed by a Yugabyte YEDIS API server.

## Synopsis

`MONITOR` is a debugging tool to see all requests that are being processed by a Yugabyte YEDIS API server. If there are multiple YEDIS API servers in the system, the command only captures the requests being processed by the server that the client is connected to. A client can issue the `MONITOR` command through the redis-cli. Once the command is issued the server will stream all requests (except `config` commands) that are processed at the server. 

## Return value

Returns a status string, followed by an unending stream of commands that are being executed by the YEDIS server. To exit, the client is expected to `Control-C` out.

## Examples

```sh
$ MONITOR
```

```
"OK"
```

```
15319400354.989768 [0 127.0.0.1:37106] "set" "k1" "v1"
15319400357.741004 [0 127.0.0.1:37106] "get" "k1"
15319400361.280308 [0 127.0.0.1:37106] "set" "k2" "v2"
15319400363.819526 [0 127.0.0.1:37106] "get" "v2"
15319400386.887508 [0 127.0.0.1:37106] "select" "2"
15319400392.983032 [2 127.0.0.1:37106] "get" "k1"
15319400405.534111 [2 127.0.0.1:37106] "set" "k1"
```

## See also

[`config`](../config/)
