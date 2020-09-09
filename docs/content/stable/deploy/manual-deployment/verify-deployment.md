---
title: Verify deployment
headerTitle: Verify deployment
linkTitle: 5. Verify deployment
description: Verify deployment of your YugabyteDB cluster
aliases:
  - /deploy/manual-deployment/verify-deployment
block_indexing: true
menu:
  stable:
    identifier: deploy-manual-deployment-verify-deployment
    parent: deploy-manual-deployment
    weight: 615
isTocNested: true
showAsideToc: true
---

We now have a cluster/universe on six nodes with a replication factor of `3`. Assume their IP addresses are `172.151.17.130`, `172.151.17.220`, `172.151.17.140`, `172.151.17.150`, `172.151.17.160` and `172.151.17.170`. YB-Master servers are running on only the first three of these nodes.

## [Optional] Setup YEDIS API

{{< note title="Note" >}}

If you want this cluster to be able to support Redis clients, you **must** perform this step.

{{< /note >}}

While the YCQL and YSQL APIs are turned on by default after all of the YB-TServers start, the Redis-compatible YEDIS API is off by default. If you want this cluster to be able to support Redis clients, run the following command from any of the 4 instances. The command below will add the special Redis table into the DB and also start the YEDIS server on port 6379 on all instances.

```sh
$ ./bin/yb-admin --master_addresses 172.151.17.130:7100,172.151.17.220:7100,172.151.17.140:7100 setup_redis_table
```

## View the master UI dashboard

You should now be able to view the master dashboard on the ip address of any master. In our example, this is one of the following URLs:

- `http://172.151.17.130:7000`
- `http://172.151.17.220:7000`
- `http://172.151.17.140:7000`

{{< tip title="Tip" >}}

If this is a public cloud deployment, remember to use the public ip for the nodes, or a http proxy to view these pages.

{{< /tip >}}

## Connect clients

- Clients can connect to YSQL API at

```sh
172.151.17.130:5433,172.151.17.220:5433,172.151.17.140:5433,172.151.17.150:5433,172.151.17.160:5433,172.151.17.170:5433
```

- Clients can connect to YCQL API at

```sh
172.151.17.130:9042,172.151.17.220:9042,172.151.17.140:9042,172.151.17.150:9042,172.151.17.160:9042,172.151.17.170:9042
```

- Clients can connect to YEDIS API at

```sh
172.151.17.130:6379,172.151.17.220:6379,172.151.17.140:6379,172.151.17.150:6379,172.151.17.160:6379,172.151.17.170:6379
```

## Default ports reference

The above deployment uses the various default ports listed below.

Service | Type | Port
--------|------| -------
`yb-master` | rpc | 7100
`yb-master` | admin web server | 7000
`yb-tserver` | rpc | 9100
`yb-tserver` | admin web server | 9000
`ycql` | rpc | 9042
`ycql` | admin web server | 12000
`yedis` | rpc | 6379
`yedis` | admin web server | 11000
`ysql` | rpc | 5433
`ysql` | admin web server | 13000
