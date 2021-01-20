---
title: yugabyted reference
headerTitle: yugabyted
linkTitle: yugabyted
description: Use yugabyted to run single-node YugabyteDB clusters.
menu:
  latest:
    identifier: yugabyted
    parent: configuration
    weight: 2451
isTocNested: true
showAsideToc: true
---

`yugabyted` is a new database server that acts as a parent server across the [`yb-tserver`](../yb-tserver) and [`yb-master`](../yb-master) servers. Since its inception, YugabyteDB has relied on a 2-server architecture with YB-TServers managing the data and YB-Masters managing the metadata. However, this can introduce a burden on new users who want to get started right away. yugabyted is the answer to this user need. It also adds a new UI similar to the Yugabyte Platform UI so that users can experience a richer data placement map and metrics dashboard.

The `yugabyted` executable file is located in the YugabyteDB home's `bin` directory. 

{{< note title="Note" >}}

- yugabyted currently supports both single-node and multi-node clusters (using the `join` option in the `start` command). However, ability to create multi-node clusters is currently under BETA.

- yugabytedb is not recommended for production deployments at this time. For production deployments with fully-distributed multi-node clusters, use [`yb-tserver`](../yb-tserver) and [`yb-master`](../yb-master) directly using the [Deploy](../../../deploy) docs.

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
- [destroy](#destroy)
- [status](#status)
- [version](#version)
- [collect_logs](#collect-logs)
- [connect](#connect)
- [demo](#demo)

-----

### start

Use the `yugabyted start` command to start a one-node YugabyteDB cluster in your local environment. This one-node cluster includes [`yb-tserver`](../yb-tserver) and [`yb-master`](../yb-master) services.

#### Syntax

```sh
Usage: yugabyted start [-h] [--config CONFIG] [--data_dir DATA_DIR]
                                [--base_dir BASE_DIR] [--log_dir LOG_DIR]
                                [--ycql_port YCQL_PORT]
                                [--ysql_port YSQL_PORT]
                                [--master_rpc_port MASTER_RPC_PORT]
                                [--tserver_rpc_port TSERVER_RPC_PORT]
                                [--master_webserver_port MASTER_WEBSERVER_PORT]
                                [--tserver_webserver_port TSERVER_WEBSERVER_PORT]
                                [--webserver_port WEBSERVER_PORT]
                                [--listen LISTEN] [--join JOIN]
                                [--daemon BOOL] [--callhome BOOL] [--ui BOOL]
                                [--initial_scripts_dir INITIAL_SCRIPTS_DIR]
```

#### Flags

##### -h, --help

Print the command line help and exit.

##### --config *config-file*

The path to the configuration file.

##### --data_dir *data-directory*

The directory where yugabyted stores data. Must be an absolute path.

##### --base_dir *base-directory*

The directory where yugabyted stores data, conf and logs. Must be an absolute path.

##### --log_dir *log-directory*

The directory to store yugabyted logs. Must be an absolute path.

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

##### --listen *bind-ip*

The IP address or localhost name to which `yugabyted` will listen.

##### --join *master-ip*

{{< note title="Note" >}}

This feature is currently in [BETA](../../../faq/general/#what-is-the-definition-of-the-beta-feature-tag).

{{< /note >}}

The IP address of the existing `yugabyted` server to which the new `yugabyted` server will join.

##### --daemon *bool*

Enable or disable running `yugabyted` in the background as a daemon. Does not persist on restart. Default is `true`.

##### --callhome *bool*

Enable or disable the "call home" feature that sends analytics data to Yugabyte. Default is `true`.

##### --ui *bool*

Enable or disable the webserver UI. Default is `false`.

##### --master_flags *master_flags*

Specify extra [master flags](../../../reference/configuration/yb-master#configuration-flags) as a set of key value pairs. Format (key=value,key=value).

##### --tserver_flags *tserver_flags*

Specify extra [tserver flags](../../../reference/configuration/yb-tserver#configuration-flags) as a set of key value pairs. Format (key=value,key=value).

##### --ysql_enable_auth *bool*

Enable or disable YSQL Authentication. Default is `false`.
If the `YSQL_PASSWORD` environment variable exists then, authentication mode is automatically changed to enforced.

##### --use_cassandra_authentication *bool*

Enable or disable YCQL Authentication. Default is `false`.
If the `YCQL_USER` or `YCQL_PASSWORD` environment variables exist then, authentication mode is automatically changed to enforced.

**Note**
- The corresponding environment variables have higher priority than the command-line flags.

##### --initial_scripts_dir *initial-scripts-dir*

The directory from where yugabyted reads initialization scripts.
The format will be: For YSQL - `.sql`, For YCQL - `.cql`.
Initialization scripts will be executed in sorted name order.

-----

### stop

Use the `yugabyted stop` command to stop a YugabyteDB cluster.

#### Syntax

```sh
Usage: yugabyted stop [-h] [--config CONFIG] [--data_dir DATA_DIR]
                               [--base_dir BASE_DIR]
```

#### Flags

##### -h | --help

Print the command line help and exit.
  
##### --config *config-file*

The path to the configuration file of the yugabyted server that needs to be stopped.
  
##### --data_dir *data-directory*

The data directory for the yugabtyed server that needs to be stopped.

##### --base_dir *base-directory*

The base directory for the yugabtyed server that needs to be stopped.

-----

### status

Use the `yugabyted status` command to check the status.

#### Syntax

```
Usage: yugabyted status [-h] [--config CONFIG] [--data_dir DATA_DIR]
                                 [--base_dir BASE_DIR]
```

#### Flags

##### -h | --help

Print the command line help and exit.
  
##### --config *config-file*

The path to the configuration file of the yugabyted server whose status is desired.
  
##### --data_dir *data-directory*

The data directory for the yugabtyed server whose status is desired.

##### --base_dir *base-directory*

The base directory for the yugabtyed server that whose status is desired.

-----

### version

Use the `yugabyted version` command to check the version number.

#### Syntax

```
Usage: yugabyted version [-h] [--config CONFIG] [--data_dir DATA_DIR]
                                  [--base_dir BASE_DIR]
```

#### Flags

##### -h | --help

Print the command line help and exit.
  
##### --config *config-file*

The path to the configuration file of the yugabyted server whose version is desired.
  
##### --data_dir *data-directory*

The data directory for the yugabtyed server whose version is desired.

##### --base_dir *base-directory*

The base directory for the yugabtyed server that whose version is desired.

-----

### collect_logs

Use the `yugabyted collect_logs` command to generate a zipped file with all logs.

#### Syntax

```
Usage: yugabyted collect_logs [-h] [--config CONFIG]
                                       [--data_dir DATA_DIR]
                                       [--base_dir BASE_DIR]
```

#### Flags

##### -h | --help

Print the command line help and exit.
  
##### --config *config-file*

The path to the configuration file of the yugabyted server whose logs are desired.
  
##### --data_dir *data-directory*

The data directory for the yugabtyed server whose logs are desired.

##### --base_dir *base-directory*

The base directory for the yugabtyed server that whose logs are desired.

-----

### connect

Use the `yugabyted connect` command to connect to the cluster with `ysqlsh` or `ycqlsh` cli.

#### Syntax

```
Usage: yugabyted connect [-h] {ycql,ysql} ...

Commands:
  {ycql,ysql}
    ycql       Use YCQL through the CLI.
    ysql       Use YSQL through the CLI.
```

#### Flags

##### -h | --help

Print the command line help and exit.
  
##### --ysql

Connect with `ysqlsh` cli.
  
##### --ycql

Connect with `ycqlsh` cli.

-----

### demo

Use the `yugabyted demo connect` command to start YugabyteDB with the [northwind sample dataset](../../../sample-data/northwind/). 

#### Syntax

```
Usage: yugabyted demo [-h] {connect,destroy} ...
```

#### Flags

##### -h | --help

Print the help message and exit.

##### connect

Loads the `northwind` sample dataset into a new `yb_demo_northwind` SQL database and then opens up `ysqlsh` prompt for the same database. Skips the data load if data is already loaded.

##### destroy

Shuts down the yugabyted single-node cluster and removes data, configuration, and log directories.

Deletes the `yb_demo_northwind` northwind database.

-----

## Environment Variables

### For YSQL:  `YSQL_USER` `YSQL_PASSWORD` `YSQL_DB`

Set `YSQL_PASSWORD` to use the cluster in enforced authentication mode.

Combinations of environment variables and their uses. 

- `YSQL_PASSWORD`
   
  Update the default yugabyte user's password.

- `YSQL_PASSWORD, YSQL_DB`

  Update the default yugabyte user's password and create `YSQL_DB` named DB.

- `YSQL_PASSWORD, YSQL_USER`

  Create `YSQL_USER` named user and DB with password `YSQL_PASSWORD`.

- `YSQL_USER`

  Create `YSQL_USER` named user and DB with password `YSQL_USER`.

- `YSQL_USER, YSQL_DB`

  Create `YSQL_USER` named user with password `YSQL_USER` and `YSQL_DB` named DB.

- `YSQL_DB`

  Create `YSQL_DB` named DB.

- `YSQL_USER, YSQL_PASSWORD, YSQL_DB`
  
  Create `YSQL_USER` named user with password `YSQL_PASSWORD` and `YSQL_DB` named DB.

### For YCQL:  `YCQL_USER` `YCQL_PASSWORD` `YCQL_KEYSPACE`

Set `YCQL_USER` or `YCQL_PASSWORD` to use the cluster in enforced authentication mode.

Combinations of environment variables and their uses.

- `YCQL_PASSWORD`
   
  Update the default cassandra user's password.

- `YCQL_PASSWORD, YCQL_KEYSPACE`

  Update the default cassandra user's password and create `YCQL_KEYSPACE` named keyspace.

- `YCQL_PASSWORD, YCQL_USER`

  Create `YCQL_USER` named user and DB with password `YCQL_PASSWORD`.

- `YCQL_USER`

  Create `YCQL_USER` named user and DB with password `YCQL_USER`.

- `YCQL_USER, YCQL_KEYSPACE`

  Create `YCQL_USER` named user with password `YCQL_USER` and `YCQL_USER` named keyspace.

- `YCQL_KEYSPACE`

  Create `YCQL_KEYSPACE` named keyspace.

- `YCQL_USER, YCQL_PASSWORD, YCQL_KEYSPACE`
  
  Create `YCQL_USER` named user with password `YCQL_PASSWORD` and `YCQL_KEYSPACE` named keyspace.

**Note**
- In the case of multi-node deployment, all nodes should have similar environment variables. 
- Changing the values of the environment variables after the first run has no effect.
-----

## Examples

### Create a single-node cluster

Create a single-node cluster with a given base dir and listen address. Note the need to provide a fully-qualified directory path for the base dir parameter.

```sh
bin/yugabyted start --base_dir=/Users/username/yugabyte-2.3.3.0/data1 --listen=127.0.0.1
```

### Pass additional flags to tserver

Create a single-node cluster and set additional flags to the yb-tserver process.

```sh
bin/yugabyted start --tserver_flags="pg_yb_session_timeout_ms=1200000,ysql_max_connections=400"
```

### Create a multi-node cluster

Add two more nodes to the cluster using the `join` option.

```sh
bin/yugabyted start --base_dir=/Users/username/yugabyte-2.3.3.0/data2 --listen=127.0.0.2 --join=127.0.0.1
bin/yugabyted start --base_dir=/Users/username/yugabyte-2.3.3.0/data3 --listen=127.0.0.3 --join=127.0.0.1
```

### Destroy a multi-node cluster

Destroy the above multi-node cluster.

```sh
bin/yugabyted destroy --base_dir=/Users/username/yugabyte-2.3.3.0/data1
bin/yugabyted destroy --base_dir=/Users/username/yugabyte-2.3.3.0/data2
bin/yugabyted destroy --base_dir=/Users/username/yugabyte-2.3.3.0/data1
```
