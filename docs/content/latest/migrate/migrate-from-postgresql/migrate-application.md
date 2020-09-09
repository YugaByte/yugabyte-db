---
title: Migrate a PostgreSQL application
headerTitle: Migrate a PostgreSQL application
linkTitle: Migrate a PostgreSQL application
description: How to migrate an application written for PostgreSQL to YugabyteDB.
menu:
  latest:
    identifier: migrate-postgresql-app
    parent: migrate-from-postgresql
    weight: 740
isTocNested: false
showAsideToc: true
---

This section outlines the recommended changes for porting an existing PostgreSQL application to YugabyteDB.

## Retry transactions on conflicts

Currently (as of YugabyteDB v2.2), only a subset of transactions that get aborted due to an internal conflict are retried transparently. YugabyteDB uses the error code 40001 (serialization_failure) for retryable transaction conflict errors. We recommend retrying the transactions from the application upon encountering these errors.

{{< note title="Note" >}}
This is the state as of YugabyteDB v2.2, reducing transaction conflicts by transparently handling retries of most transactions transparently is work in progress.
{{< /note >}}


## Distribute load evenly across the cluster

All nodes (YB-TServers) in the cluster are identical and are capable of handling queries. However, the client drivers of PostgreSQL are designed to communicate only with a single endpoint (node). In order to utilize all the nodes of the cluster evenly, the queries from the application would need to be distributed uniformly across all nodes of the cluster. There are two ways to accomplish this:

* **Use a load balancer** to front all the nodes of the cluster. The load balancer should be set to round-robin all requests across the nodes in the cluster.

* **Modify the application to distribute queries across nodes** in the cluster. In this scenario, typically a DNS entry is used to maintain the list of nodes in the cluster. The application periodically refreshes this list, and distributes the queries across the various nodes of the cluster in a round robin manner.

## Handling large number of connections

There are many applications where handling a large number of client connections is critical. There are two strategies to deal with this.

* **Evenly distribute queries across nodes:** Every node (YB-Tserver process) of a YugabyteDB cluster has a limit on the number of connections it can handle, by default this number is 300 connections. While this number can be increased a bit depending on the use case, it is recommended to distribute the queries across the different nodes in the cluster. As an example, a 10 node cluster consisting of 16 vCPU per node can handle 3000 connections.

* **Use a connection pool:** Use a connection pool in your application such as the Hikari pool. Using a connection pool drastically reduces the number of connections by multiplexing a large number of logical client connections onto a smaller number of physical connections across the nodes of the YugabyteDB cluster.

* **Increase number of nodes in cluster:**  Note that the number of connections to a YugabyteDB cluster scales linearly with the number of nodes in the cluster. By deploying more nodes with smaller vCPUs per node, it may be possible to get more connections. As an example, a 10 node cluster consisting of 32 vCPU per node can handle 3000 connections. If more connections are desirable, deploying a 20 node cluster with 16 vCPUs per node (which is equivalent to the 10 node, 32 vCPU cluster) can handle 6000 connections.

## Use PREPARED statements

Prepared statements are critical to achieve good performance in YugabyteDB because they avoid re-parsing (and typically re-planning) on every query. Most SQL drivers will auto-prepare statements, in these cases, it may not be necessary to explicitly prepare statements. 

In cases when the driver does not auto-prepare, use an explicit prepared statement where possible. This can be done programmatically in the case of many drivers. In scenarios where the driver does not have support for preparing statements (for example, the Python psycopg2 driver), the queries can be optimized on each server by using the PREPARE <plan> AS <plan name> feature.

For example, if you have two tables t1 and t2 both with two columns k (primary key) and v:

```
CREATE TABLE t1 (k VARCHAR PRIMARY KEY, v VARCHAR);

CREATE TABLE t2 (k VARCHAR PRIMARY KEY, v VARCHAR);
```

Now, consider the following code snippet which repeatedly makes SELECT queries that are not prepared.

```
for idx in range(num_rows):
  cur.execute("SELECT * from t1, t2 " + 
              "  WHERE t1.k = t2.k AND t1.v = %s LIMIT 1"
              , ("k1"))
```

Since the Python psycopg2 driver does not support prepared bind statements (using a cursor.prepare() API), the explicit PREPARE statement as in the case of PostgreSQL. The above code snippet can be optimized by changing the above query to the following equivalent query.

```
cur.execute("PREPARE myplan as " + 
            "  SELECT * from t1, t2 " +
            "  WHERE t1.k = t2.k AND $1 = $2 LIMIT 1")
  for idx in range(num_rows):
    cur.execute("""EXECUTE myplan(%s, %s)""", ("foo", "foo"))
```
