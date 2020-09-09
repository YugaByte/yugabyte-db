---
title: Default ports reference
headerTitle: Default ports
linkTitle: Default ports
description: Default ports for YugabyteDB including client APIs, RPC communication, and monitoring.
section: REFERENCE
block_indexing: true
menu:
  stable:
    identifier: default-ports
    parent: configuration
    weight: 2740
aliases:
  - /stable/reference/default-ports
isTocNested: true
showAsideToc: true
---

## Client APIs

Application clients connect to these addresses.

| API     | Port  | Server | Flag (default)           |
| ------- | ----- | ------- |------------------------------------------|
| ysql    | 5433  | yb-tserver | [`--pgsql_proxy_bind_address 0.0.0.0:5433`](../yb-tserver/#pgsql-proxy-bind-address) |
| ycql    | 9042  | yb-tserver | [`--cql_proxy_bind_address 0.0.0.0:9042`](../yb-tserver/#cql-proxy-bind-address)   |
| yedis   | 6379  | yb-tserver | [`--redis_proxy_bind_address 0.0.0.0:6379`](../yb-tserver/#redis-proxy-bind-address) |

## Internode RPC communication

Internode (server-to-server or node-to-node) communication is managed using RPC calls on these addresses.

| Server    | Port | Flag (default)                              |
| ---------- | ---- | ------------------------------------------------------------ |
| yb-master  | 7100 |  [`--rpc_bind_addresses 0.0.0.0:7100`](../yb-master/#rpc-bind-addresses) |
| yb-tserver | 9100 |  [`--rpc_bind_addresses 0.0.0.0:9100`](../yb-tserver/#rpc-bind-addresses)<br/>[`--tserver_master_addrs 0.0.0.0:7100`](../yb-tserver/#tserver-master-addrs)<br/>[`--server_broadcast_addresses 0.0.0.0:9100`](../yb-tserver/#server-broadcast-addresses) |

If you want to log into the machines running these servers, then the ssh port `22` should be opened as well.

## Admin web server

Admin web server UI can be viewed at these addresses.

| Server    | Port  | Flag (default)                             |
| ---------- | ----- | ------------------------------------------------------------ |
| yb-master  | 7000  |  [`--webserver_interface 0.0.0.0`](../yb-master/#webserver-interface)<br>[`--webserver_port 7000`](../yb-master/#webserver-port) |
| yb-tserver | 9000  |  [`--webserver_interface 0.0.0.0`](../yb-master/#webserver-interface)<br>[`--webserver_port 9000`](../yb-master/#webserver-port) |

## Prometheus monitoring

YugabyteDB servers expose time-series performance metrics in the [Prometheus exposition format](https://prometheus.io/docs/instrumenting/exposition_formats/#text-based-format) on multiple HTTP endpoints. These endpoints have the following structure.

```
<target>/prometheus-metrics
```

Following is the list of targets available.

### Servers

Use the following targets to monitor `yb-tserver` and `yb-master` server metrics.

| Server     | Target                      |
| ---------- | --------------------------- |
| yb-master  | `<yb-master-address>:7000`  |  
| yb-tserver | `<yb-tserver-address>:9000` |

### APIs

Use the following `yb-tserver` targets for the various API metrics.

| API     | Target
| ------- | ------------------------- |
| ysql    | `<yb-tserver-address>:13000` |
| ycql    | `<yb-tserver-address>:12000` |
| yedis   | `<yb-tserver-address>:11000` |

For a quick tutorial on using Prometheus with YugabyteDB, see [Observability with Prometheus](../../../explore/observability).
