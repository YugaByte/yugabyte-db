---
title: yugabyted reference
headerTitle: yugabyted
linkTitle: yugabyted
description: Use yugabyted to simplify creating, running, and managing yb-tserver and yb-master servers.
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
block_indexing: true
menu:
  v2.1:
    identifier: yugabyted
    parent: configuration
    weight: 2451
isTocNested: true
showAsideToc: true
---

`yugabyted` is a new database server that acts as a parent server across the [`yb-tserver`](../yb-tserver) and [`yb-master`](../yb-master) servers. Since its inception, YugabyteDB has relied on a 2-server architecture with YB-TServers managing the data and YB-Masters managing the metadata. However, this can introduce a burden on new users who want to get started right away. yugabyted is the answer to this user need. It also adds a new UI similar to the Yugabyte Platform UI so that users can experience a richer data placement map and metrics dashboard.

The `yugabyted` executable file is located in the YugabyteDB home's `bin` directory. 

{{< note title="Note" >}}

yugabyted currently supports creating a 1-node cluster only. Ability to create multi-node clusters is under active development. 

- For local multi-node clusters, use [`yb-ctl`](../../../admin/yb-ctl). 

- For production deployments with fully-distributed multi-node clusters, use [`yb-tserver`](../yb-tserver) and [`yb-master`](../yb-master).

{{< /note >}}

## Syntax

```sh
yugabyted [-h] [ <command> ] [ <flags> ]
```

- *command*: command to run
- *flags*: one or more flags, separated by spaces.

### Example 

```sh
$ ./bin/yugabyted start
```

### Online help

You can access the overview command line help for `yugabyted` by running one of the following examples from the YugabyteDB home.

```sh
$ ./bin/yugabyted -h
```

```sh
$ ./bin/yugabyted -help
```

For help with specific `yugabyted` commands, run 'yugabyted [ command ] -h'. For example, you can print the command line help for the `yugabyted start` command by running the following:

```sh
$ ./bin/yugabyted start -h
```

## Commands

The following commands are available:

- [start](#start)
- [stop](#stop)
- [status](#status)
- [version](#version)
- [demo](#demo)

-----

### start

Use the `yugabyted start` command to start a one-node YugabyteDB cluster in your local environment. This one-node cluster includes [`yb-tserver`](../yb-tserver) and [`yb-master`](../yb-master) services.

#### Syntax

```sh
 yugabyted start
   [ -h | --help ] 
   [ --config <config-file> ]
   [ --data_dir <data-dir> ]
   [ --log_dir <log-dir> ]  
   [ --ycql_port <ycql-port> ]
   [ --ysql_port <ysql-port> ]
   [ --master_rpc_port <master-rpc-port> ]
   [ --tserver_rpc_port <tserver-rpc-port> ]
   [ --master_webserver_port <master-webserver-port> ]
   [ --tserver_webserver_port <tserver-webserver-port> ]
   [ --webserver_port <webserver-port> ]
   [ --bind_ip BIND_IP ] 
   [ --daemon <bool> ]
   [ --callhome <bool> ] 
   [ --ui <bool> ]
```

#### Flags

##### -h, --help

Print the commmand line help and exit.

##### --config *config-file*

The path to the configuration file.

##### --data_dir *data-directory*

The directory where YugabyteDB stores data.

##### --log_dir *log-directory*

The directory to store YugabyteDB logs.

##### --ycql_port *ycql-port*

The port on which YCQL will run.

##### --ysql_port *ysql-port*

The port on which YSQL will run.

##### --master_rpc_port *master-rpc-port*

The port on which YB-Master will listen for RPC calls.

##### --tserver_rpc_port *tserver-rpc-port*

The port on which YB-TServer will listen for RPC calls.

##### --master_webserver_port *master-webserver-port*

The port on which YB-Master webserver will run.

##### --tserver_webserver_port *tserver-webserver-port*

The port on which YB-TServer webserver will run.

##### --webserver_port *webserver-port*

The port on which main webserver will run.

##### --bind_ip *bind-ip*

The IP address to which `yugabyted` processes will bind.

##### --daemon *bool*

Enable or disable running `yugabyted` in the background as a daemon. Does not persist on restart. Default is `true`.

##### --callhome *bool*

Enable or disable the "call home" feature that sends analytics data to Yugabyte. Default is `true`.

##### --ui *bool*

Enable or disable the webserver UI. Default is `true`.

-----
### stop

Use the `yugabted stop` command to stop a YugabyteDB cluster.

#### Syntax

```sh
yugabyted stop [ -h ] [ --config <config-file> ] [ --data_dir <data-directory> ]
```

#### Flags

##### -h | --help

Print the command line help and exit.
  
##### --config *config-file*

The path to the YugabyteDB configuration file.
  
##### --data_dir *data-directory*

The directory where YugabyteDB will store data.

-----

### status

Use the `yugabyted status` command to check the status.

#### Syntax

```
yugabyted status [ -h | --help ] [ --config <config-file> ] [ --data_dir <data-directory> ]
```

#### Flags

##### -h --help

Print the command line help and exit.

##### --config *config-file*

The path to the YugabyteDB configuration file.

##### --data_dir *data-directory*

The directory where YugabyteDB stores data.

-----

### version

Use the `yugabyted version` command to check the version number.

#### Syntax

```
yugabyted version [ -h | --help ] [ --config <config-file> ] [ --data_dir <data-directory> ]
```

#### Flags

##### -h | --help

Print the help message and exit.

##### --config *config-file*

The path to the YugabyteDB configuration file.

##### --data_dir *data-directory*

The directory where YugabyteDB stores data.

-----

### demo

Use the `yugabyted demo` command to start YugabyteDB with a retail demo database. Get started with YSQL by using the [Explore YSQL](../../../quick-start/explore-ysql) tutorial in the [Quick start](../../../quick-start/) guide.

{{< note title="Note" >}}

When you quit the demo instance, the retail demo database is deleted and any changes you've made are lost.

{{< /note >}}

#### Syntax

```
yugabyted demo [ -h | -help ] [ --config <config-file> ] [ --data_dir <data-directory> ]
```

#### Flags

##### -h | --help

Print the help message and exit.

##### --config *config-file*

The path to the YugabyteDB configuration file.

##### --data_dir *data_directory*

The directory where YugabyteDB stores data.
