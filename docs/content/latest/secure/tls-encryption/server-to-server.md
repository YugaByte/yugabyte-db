---
title: Enable server-to-server encryption
headerTitle: Enable server-to-server encryption
linkTitle: Enable server-to-server encryption
description: Enable server-to-server encryption (using TLS) for intranode communication.
headcontent: Enable server-to-server encryption (using TLS) for intranode communication.
image: /images/section_icons/secure/tls-encryption/server-to-server.png
aliases:
  - /secure/tls-encryption/server-to-server
menu:
  latest:
    identifier: server-to-server
    parent: tls-encryption
    weight: 20
isTocNested: true
showAsideToc: true
---


## Prerequisites

Before you can enable and use server-to-server encryption, you need to create and configure server certificates for each node of your YugabyteDB cluster. For information, see [Create server certificates](../server-certificates).

## Configure YB-Master and YB-TServer nodes

To enable server-to-server encryption using TLS, start your YB-Master and YB-TServer nodes using the following flags.

Flag                           | Node                  | Description                  |
-------------------------------|--------------------------|------------------------------|
`use_node_to_node_encryption`  | YB-Master, YB-TServer | Set to `true` to enable encryption between YugabyteDB nodes. Default value is `false`. |
`allow_insecure_connections`   | YB-Master, YB-TServer | Set to `false` to disallow any service with unencrypted communication from joining this cluster. Default value is `true`. Note that this flag requires `--use_node_to_node_encryption` to be enabled. |
`certs_dir`                    | YB-Master, YB-TServer | Optional. This directory should contain the configuration that was prepared in the a step for this node to perform encrypted communication with the other nodes. Default value for YB-Masters is `<data drive>/yb-data/master/data/certs` and for YB-TServers this location is `<data drive>/yb-data/tserver/data/certs` |

## Start the YB-Masters

You can enable access control by starting the `yb-master` services with the `--use_node_to_node_encryption=true` flag as described above. Your command should look similar to this:

```
bin/yb-master                               \
    --fs_data_dirs=<data directories>       \
    --master_addresses=<master addresses>   \
    --certs_dir=/home/centos/tls/$NODE_IP   \
    --allow_insecure_connections=false      \
    --use_node_to_node_encryption=true
```

For information on starting YB-Master nodes for a deployment, see [Start YB-Masters](../../../deploy/manual-deployment/start-masters/).

## Start the YB-TServers

You can enable access control by starting the `yb-tserver` services using the `--use_node_to_node_encryption=true` flag described above. Your command should look similar to this:

```
bin/yb-tserver                                  \
    --fs_data_dirs=<data directories>           \
    --tserver_master_addrs=<master addresses>   \
    --certs_dir /home/centos/tls/$NODE_IP       \
    --use_node_to_node_encryption=true &
```

For information on starting YB-TServers for a deployment, see [start YB-TServers](../../../deploy/manual-deployment/start-tservers/).

## Connect to the cluster

Because you have only enabled server-to-server encryption and not [client-to-server encryption](../client-to-server), you can now connect to this cluster using the YSQL shell (`ysqlsh`) or the YCQL shell (`ycqlsh`) without enabling encryption as shown here.

### YSQL

```sh
$ ./bin/ysqlsh
```

```
ysqlsh (11.2-YB-2.0.11.0-b0)
Type "help" for help

yugabyte=#
```

### YCQL

```sh
$ ./bin/ycqlsh
```

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.

ycqlsh>
```
