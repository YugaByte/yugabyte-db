---
title: Explore fault tolerance on Kubernetes
headerTitle: Fault tolerance
linkTitle: Fault tolerance
description: Simulate fault tolerance and resilience in a local three-node YugabyteDB cluster on Kubernetes (Minikube).
aliases:
  - /latest/explore/fault-tolerance-kubernetes/
menu:
  latest:
    identifier: fault-tolerance-4-kubernetes
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
    <a href="/latest/explore/fault-tolerance/docker" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker
    </a>
  </li>

  <li >
    <a href="/latest/explore/fault-tolerance/kubernetes" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Kubernetes
    </a>
  </li>

</ul>

YugabyteDB can automatically handle failures and therefore provides [high availability](../../../architecture/core-functions/high-availability/). You will create YSQL tables with a replication factor (RF) of `3` that allows a [fault tolerance](../../../architecture/concepts/docdb/replication/) of `1`. This means the cluster will remain available for both reads and writes even if one node fails. However, if another node fails, bringing the number of failures to two, then writes will become unavailable on the cluster in order to preserve data consistency.

If you haven't installed YugabyteDB yet, you can create a local YugabyteDB cluster within five minutes by following the [Quick Start](../../../quick-start/install/) guide.

## 1. Create universe

If you have a previously running local universe, destroy it using the following command.

```sh
$ helm uninstall yb-demo -n yb-demo
$ kubectl delete pvc --namespace yb-demo --all
```
Create a new YugabyteDB cluster.

```sh
$ helm install yb-demo yugabytedb/yugabyte \
--set resource.master.requests.cpu=0.5,resource.master.requests.memory=0.5Gi,\
resource.tserver.requests.cpu=0.5,resource.tserver.requests.memory=0.5Gi --namespace yb-demo
```

Check the Kubernetes dashboard to see the three YB-Master and three YB-TServer pods representing the three nodes of the cluster.

```sh
$ minikube dashboard
```

![Kubernetes Dashboard](/images/ce/kubernetes-dashboard.png)

## 2. Check cluster status with Admin UI

To check the cluster status, you need to access the Admin UI on port `7000` exposed by the `yb-master-ui` service. In order to do so, you need to find the port forward the port.

```sh
$ kubectl --namespace yb-demo port-forward svc/yb-master-ui 7000:7000
```

Now, you can view the [yb-master-0 Admin UI](../../../reference/configuration/yb-master/#admin-ui) is available at http://localhost:7000.

## 3. Connect to YugabyteDB Shell

Connect to `ycqlsh` on node `1`.

```sh
$ kubectl -n yb-demo exec -it yb-tserver-0 -- ycqlsh yb-tserver-0
```

```
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

## 4. Insert data through a node

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

## 5. Read data through another node

Let us now query the data from node `3`.

```sh
$ kubectl -n yb-demo exec -it yb-tserver-2 -- ycqlsh yb-tserver-2
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

```sql
ycqlsh> exit;
```

## 6. Verify one node failure has no impact

This cluster was created with a replication factor of `3` and hence needs only two replicas to make consensus. Therefore, it is resilient to one failure without any data loss. Let us simulate node `3` failure.

```sh
$ kubectl -n yb-demo delete pod yb-tserver-2
```

Now running the status command should would show that the `yb-tserver-2` pod is `Terminating`.

```sh
$ kubectl -n yb-demo get pods
```

```
NAME           READY     STATUS        RESTARTS   AGE
yb-master-0    1/1       Running       0          33m
yb-master-1    1/1       Running       0          33m
yb-master-2    1/1       Running       0          33m
yb-tserver-0   1/1       Running       1          33m
yb-tserver-1   1/1       Running       1          33m
yb-tserver-2   1/1       Terminating   0          33m
```

Now connect to node `2`.

```sh
$ kubectl -n yb-demo exec -it yb-tserver-1 -- ycqlsh yb-tserver-1
```

Let us insert some data to ensure that the loss of a node hasn't impacted the ability of the universe to take writes.

```sql
ycqlsh> INSERT INTO users.profile (id, email, password, profile) VALUES 
  (3000, 'austin.powers@yugabyte.com', 'imGroovy',
   {'firstname': 'Austin', 'lastname': 'Powers'});
```

Now query the data. We see that all the data inserted so far is returned and the loss of the node has no impact on data integrity.

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

## 7. Verify that Kubernetes brought back the failed node

We can now check the cluster status to verify that Kubernetes has indeed brought back the `yb-tserver-2` node that had failed before. This is because the replica count currently effective in Kubernetes for the `yb-tserver` StatefulSet is `3` and there were only two nodes remaining after one node failure.

```sh
$ kubectl -n yb-demo get pods
```

```
NAME           READY     STATUS    RESTARTS   AGE
yb-master-0    1/1       Running   0          34m
yb-master-1    1/1       Running   0          34m
yb-master-2    1/1       Running   0          34m
yb-tserver-0   1/1       Running   1          34m
yb-tserver-1   1/1       Running   1          34m
yb-tserver-2   1/1       Running   0          7s
```

YugabyteDB's fault tolerance when combined with Kubernetes' automated operations ensures that distributed applications can be run with ease while ensuring extreme data resilience.

## 8. Clean up (optional)

Optionally, you can shut down the local cluster created in Step 1.

```sh
$ helm uninstall yb-demo -n yb-demo
$ kubectl delete pvc --namespace yb-demo --all
