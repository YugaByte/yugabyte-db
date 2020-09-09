---
title: Explore fault tolerance on Docker
headerTitle: Fault tolerance
linkTitle: Fault tolerance
description: Simulate fault tolerance and resilience in a local three-node YugabyteDB cluster on Docker.
block_indexing: true
menu:
  v2.1:
    identifier: fault-tolerance-3-docker
    parent: explore
    weight: 215
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">

  <li >
    <a href="/latest/explore/fault-tolerance/macos" class="nav-link">
      <i class="fab fa-apple" aria-hidden="true"></i>
      macOS
    </a>
  </li>

  <li >
    <a href="/latest/explore/fault-tolerance/linux" class="nav-link">
      <i class="fab fa-linux" aria-hidden="true"></i>
      Linux
    </a>
  </li>

  <li >
    <a href="/latest/explore/fault-tolerance/docker" class="nav-link active">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/latest/explore/fault-tolerance/kubernetes" class="nav-link">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

YugabyteDB can automatically handle failures and therefore provides [high availability](../../../architecture/core-functions/high-availability/). You will create YSQL tables with a replication factor of `3` that allows a [fault tolerance](../../../architecture/concepts/docdb/replication/) of 1. This means the cluster will remain available for both reads and writes even if one node fails. However, if another node fails bringing the number of failures to two, then writes will become unavailable on the cluster in order to preserve data consistency.

## Prerequisite

Install a local YugabyteDB universe on Docker using the steps below.

```sh
mkdir ~/yugabyte && cd ~/yugabyte
wget https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/bin/yb-docker-ctl && chmod +x yb-docker-ctl
docker pull yugabytedb/yugabyte
```

## 1. Create universe

If you have a previously running local universe, destroy it using the following command.

```sh
$ ./yb-docker-ctl destroy
```

Start a new local universe with replication factor of `5`.

```sh
$ ./yb-docker-ctl create --rf 5
```

Connect to `ycqlsh` on node `1`.

```sh
$ docker exec -it yb-tserver-n1 /home/yugabyte/bin/ycqlsh
```

```sh
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh>
```

Create a keyspace and a table.

```sql
ycqlsh> CREATE KEYSPACE users;
```

```sql
ycqlsh> CREATE TABLE users.profile (id bigint PRIMARY KEY,
	                               email text,
	                               password text,
	                               profile frozen<map<text, text>>);
```

## 2. Insert data through a node

Now insert some data by typing the following into `ycqlsh` shell.

```sql
ycqlsh> INSERT INTO users.profile (id, email, password, profile) VALUES
  (1000, 'james.bond@yugabyte.com', 'licensed2Kill',
   {'firstname': 'James', 'lastname': 'Bond', 'nickname': '007'}
  );
```

```sql
ycqlsh> INSERT INTO users.profile (id, email, password, profile) VALUES
  (2000, 'sherlock.holmes@yugabyte.com', 'itsElementary',
   {'firstname': 'Sherlock', 'lastname': 'Holmes'}
  );

```

Query all the rows.

```sql
ycqlsh> SELECT email, profile FROM users.profile;
```

```
 email                        | profile
------------------------------+---------------------------------------------------------------
      james.bond@yugabyte.com | {'firstname': 'James', 'lastname': 'Bond', 'nickname': '007'}
 sherlock.holmes@yugabyte.com |               {'firstname': 'Sherlock', 'lastname': 'Holmes'}

(2 rows)
```

## 3. Read data through another node

Let us now query the data from node `5`.

```sh
$ docker exec -it yb-tserver-n5 /home/yugabyte/bin/ycqlsh
```

```sql
ycqlsh> SELECT email, profile FROM users.profile;
```

```
 email                        | profile
------------------------------+---------------------------------------------------------------
      james.bond@yugabyte.com | {'firstname': 'James', 'lastname': 'Bond', 'nickname': '007'}
 sherlock.holmes@yugabyte.com |               {'firstname': 'Sherlock', 'lastname': 'Holmes'}

(2 rows)
```

## 4. Verify that one node failure has no impact

We have five nodes in this universe. You can verify this by running the following.

```sh
$ ./yb-docker-ctl status
```

Let us simulate node `5` failure by doing the following.

```sh
$ ./yb-docker-ctl remove_node 5
```

Now running the status command should show only four nodes:

```sh
$ ./yb-docker-ctl status
```

Now connect to node 4.

```sh
$ docker exec -it yb-tserver-n4 /home/yugabyte/bin/ycqlsh
```

Let us insert some data.

```sql
ycqlsh> INSERT INTO users.profile (id, email, password, profile) VALUES 
  (3000, 'austin.powers@yugabyte.com', 'imGroovy',
   {'firstname': 'Austin', 'lastname': 'Powers'});
```

Now query the data.

```sql
ycqlsh> SELECT email, profile FROM users.profile;
```

```
 email                        | profile
------------------------------+---------------------------------------------------------------
      james.bond@yugabyte.com | {'firstname': 'James', 'lastname': 'Bond', 'nickname': '007'}
 sherlock.holmes@yugabyte.com |               {'firstname': 'Sherlock', 'lastname': 'Holmes'}
   austin.powers@yugabyte.com |                 {'firstname': 'Austin', 'lastname': 'Powers'}

(3 rows)
```

## 5. Verify that second node failure has no impact

This cluster was created with a replication factor of `5` and hence needs only three replicas to make consensus. Therefore, it is resilient to 2 failures without any data loss. Let us simulate another node failure.

```sh
$ ./yb-docker-ctl remove_node 1
```

We can check the status to verify:

```sh
$ ./yb-docker-ctl status
```

Now let us connect to node `2`.

```sh
$ docker exec -it yb-tserver-n2 /home/yugabyte/bin/ycqlsh
```

Insert some data.

```sql
ycqlsh> INSERT INTO users.profile (id, email, password, profile) VALUES
  (4000, 'superman@yugabyte.com', 'iCanFly',
   {'firstname': 'Clark', 'lastname': 'Kent'});
```

Run the query.

```sql
ycqlsh> SELECT email, profile FROM users.profile;
```

```
 email                        | profile
------------------------------+---------------------------------------------------------------
        superman@yugabyte.com |                    {'firstname': 'Clark', 'lastname': 'Kent'}
      james.bond@yugabyte.com | {'firstname': 'James', 'lastname': 'Bond', 'nickname': '007'}
 sherlock.holmes@yugabyte.com |               {'firstname': 'Sherlock', 'lastname': 'Holmes'}
   austin.powers@yugabyte.com |                 {'firstname': 'Austin', 'lastname': 'Powers'}

(4 rows)
```

## Step 6. Clean up (optional)

Optionally, you can shutdown the local cluster created in Step 1.

```sh
$ ./yb-docker-ctl destroy
```
