---
title: yb-ts-cli - command line tool for advanced yb-tserver operations
headerTitle: yb-ts-cli
linkTitle: yb-ts-cli
description: Use the yb-ts-cli command line utility to perform advanced YB-TServer operations.
block_indexing: true
menu:
  v2.1:
    identifier: yb-ts-cli
    parent: admin
    weight: 2466
isTocNested: true
showAsideToc: true
---

`yb-ts-cli` is a command line tool that can be used to perform an operation on a particular tablet server (`yb-tserver`). Some of the commands perform operations similar to [`yb-admin` commands](../yb-admin). The `yb-admin` commands focus on cluster administration, the `yb-ts-cli` commands apply to specific YB-TServer nodes.

`yb-ts-cli` is a binary file installed with YugabyteDB and is located in the `bin` directory of the YugabyteDB home directory.

## Syntax

```sh
yb-ts-cli [ --server_address=<host>:<port> ] <command> <flags>
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.
- *command*: The operation to be performed. See [Commands](#commands) below.
- *flags*: The flags to be applied to the command. See [Flags](#flags).

## Command line help

To display the available online help, run `yb-ts-cli` without any commands or flags at the YugabyteDB home directory.

```sh
$ ./bin/yb-ts-cli
```

## Commands

##### are_tablets_running

If all tablets are running, returns "All tablets are running".

**Syntax**

```sh
$ yb-ts-cli [ --server_address=<host>:<port> ] are_tablets_running
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

##### compact_all_tablets

Compact all tablets on the tablet server.

**Syntax**

```sh
yb-tsi-cli [ --server_address=<host>:<port> ] compact_all_tablets
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

##### compact_tablet

Compact the specified tablet on the tablet server.

**Syntax**

```sh
yb-tsi-cli [ --server_address=<host>:<port> ] compact_tablet <tablet_id>
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.
- *tablet_id*: The identifier of the tablet to compact.

##### count_intents

Print the count of uncommitted intents (or [provisional records](../../../architecture/transactions/distributed-txns/#provisional-records)). Useful for debugging transactional workloads.

**Syntax**

```sh
yb-ts-cli  [ --server_address=<host>:<port> ] count_intents
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

##### current_hybrid_time

Prints the value of the current [hybrid time](../../../architecture/transactions/single-row-transactions/#hybrid-time-as-an-mvcc-timestamp).

**Syntax**

```sh
$ ./bin/yb-ts-cli  [ --server_address=<host>:<port> ] current_hybrid_time
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

##### delete_tablet

Deletes the tablet with the specified tablet ID (`tablet_id`) and reason.

**Syntax**

```sh
$ ./bin/yb-ts-cli  [ --server_address=<host>:<port> ] delete_tablet <tablet_id> "<reason-string>"
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.
- *tablet_id*: The identifier (ID) for the tablet.
- *reason-string*: Text string providing useful information on why the tablet was deleted.

##### dump_tablet

Dump, or export, the specified tablet ID (`tablet_id`).

**Syntax**

```sh
yb-ts-cli [ --server_address=<host>:<port> ] dump_tablet <tablet_id>
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.
- *tablet_id*: The identifier (ID) for the tablet.

##### flush_all_tablets

Flush all tablets on the tablet server.

**Syntax**

```sh
yb-tsi-cli [ --server_address=<host>:<port> ] flush_all_tablets
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

##### flush_tablet

Flush the specified tablet on the tablet server.

**Syntax**

```sh
yb-tsi-cli [ --server_address=<host>:<port> ] flush_tablet <tablet_id>
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.
- *tablet_id*: The identifier of the tablet to compact.




##### list_tablets

Lists the tablets on the specified tablet server, displaying the following properties: column name, tablet ID, state, table name, partition, and schema.

**Syntax**

```sh
yb-ts-cli [ --server_address=<host>:<port> ] list_tablets
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

##### set_flag

Sets the specified configuration flag for the tablet server.

**Syntax**

```sh
$ ./bin/yb-ts-cli [ --server_address=<host>:<port> ] set_flag [ --force ] <flag> <value>
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.
- --force: Flag to to allow a change to a flag that is not explicitly marked as runtime-settable. Note that the change may be ignored on the server or may cause the server to crash, if unsafe values are provided. See [--force](#force).
- *flag*: The `yb-tserver` configuration flag (without the `--` prefix) to be set. See [`yb-tserver`](../../reference/configuration/yb-tserver/#configuration-flags)
- *value*: The value to be applied.

{{< note title="Important" >}}

The `set_flag` command changes the in-memory value of the specified flag, atomically, for a running server and can alter its behavior.  **The change does NOT persist across restarts.**

In practice, there are some flags that are runtime safe to change (runtime-settable) and some that are not. For example, the bind address of the server cannot be changed at runtime, since the server binds just once at startup. While most of the flags are probably runtime-settable, you need to review the flags and note in the configuration pages which flags are not runtime-settable. (See GitHub issue [#3534](https://github.com/yugabyte/yugabyte-db/issues/3534)).

One typical operational flow is that you can use this to modify runtime flags in memory and then out of band also modify the configuration file that the server uses to start. This allows for flags to be changed on running servers, without executing a restart of the server.

{{< /note >}}

##### status

Prints the status of the tablet server, including information on the node instance, bound RPC addresses, bound HTTP addresses, and version information.

**Syntax**

```sh
$ ./bin/yb-ts-cli [ --server_address=<host>:<port> ] status
```

- *host*:*port*: The *host* and *port* of the tablet server. Default is `localhost:9100`.

For an example, see [Return the status of a tablet server](#return-the-status-of-a-tablet-server)

## Flags

The following flags can be used, when specified, with the commands above.

##### --force

Use this flag with the [`set_flag`](#set-flag) command to allow a change to a flag that is not explicitly marked as runtime-settable. Note that the change may be ignored on the server or may cause the server to crash, if unsafe values are provided.

Default: `false`

##### --server-address

The address (*host* and *port*) of the tablet server to run against.

Default: `localhost:9100`

##### --timeout_ms

The duration, in milliseconds (ms), before the RPC request times out.

Default: `60000` (1000 ms = 1 sec)

## Examples

### Return the status of a tablet server

```sh
$ ./bin/yb-ts-cli -server_address=127.0.0.1 status
```

```json
node_instance {
  permanent_uuid: "237678d61086489991080bdfc68a28db"
  instance_seqno: 1579278624770505
}
bound_rpc_addresses {
  host: "127.0.0.1"
  port: 9100
}
bound_http_addresses {
  host: "127.0.0.1"
  port: 9000
}
version_info {
  git_hash: "83610e77c7659c7587bc0c8aea76db47ff8e2df1"
  build_hostname: "yb-macmini-6.dev.yugabyte.com"
  build_timestamp: "06 Jan 2020 17:47:22 PST"
  build_username: "jenkins"
  build_clean_repo: true
  build_id: "743"
  build_type: "RELEASE"
  version_number: "2.0.10.0"
  build_number: "4"
}
```

### Display the current hybrid time

```sh
$ ./bin/yb-ts-cli  [ --server_address=yb-tserver-1:9100 ] current_hybrid_time
```

```
6470519323472437248
```
