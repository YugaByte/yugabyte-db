---
title: Deploy a YugabyteDB cluster on Docker Swarm
headerTitle: Docker
linkTitle: Docker
description: Deploy a YugabyteDB cluster on Docker Swarm.
aliases:
  - /deploy/docker-swarm/
  - /stable/deploy/docker-swarm/
  - /stable/deploy/docker/
block_indexing: true
menu:
  stable:
    parent: deploy
    name: Docker
    identifier: docker-2-swarm
    weight: 625
type: page
isTocNested: false
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li>
    <a href="{{< relref "./docker-compose.md" >}}" class="nav-link">
      <i class="fab fa-docker" aria-hidden="true"></i>
      Docker Compose
    </a>
  </li>
  <li >
    <a href="{{< relref "./docker-swarm.md" >}}" class="nav-link active">
      <i class="fas fa-layer-group"></i>
      Docker Swarm
    </a>
  </li>
</ul>

Docker includes [swarm](https://docs.docker.com/engine/swarm/) mode for natively managing a cluster of [Docker Engines](https://docs.docker.com/engine/) called a swarm. The Docker CLI can be used create a swarm, deploy application services to a swarm, and manage swarm behavior — without using any additional orchestration software. Details on how swarm mode works are available [here](https://docs.docker.com/engine/swarm/key-concepts/).

This tutorial uses [Docker Machine](https://docs.docker.com/machine/) to create multiple nodes on your desktop. These nodes can even be on multiple machines on the cloud platform of your choice.

## Prerequisites

### Linux

- Docker Engine 1.12 or later installed using [Docker for Linux](https://docs.docker.com/engine/swarm/swarm-tutorial/#install-docker-engine-on-linux-machines).
- [Docker Machine](https://docs.docker.com/machine/install-machine/).

### macOS

- Docker Engine 1.12 or later installed using [Docker for Mac](https://docs.docker.com/docker-for-mac/). Docker Machine is already included with Docker for Mac.

- VirtualBox 5.2 or later for creating the swarm nodes.

### Windows

- Docker Engine 1.12 or later installed using [Docker for Windows](https://docs.docker.com/docker-for-mac/). Docker Machine is already included with Docker for Windows.

- [Microsoft Hyper-V driver](https://docs.docker.com/machine/drivers/hyper-v/) for creating the swarm nodes.

As noted in the [Docker documentation](https://docs.docker.com/engine/swarm/swarm-tutorial/#use-docker-for-mac-or-docker-for-windows), the host on which Docker for Mac or Docker for Windows is installed does not itself participate in the swarm. The included version of Docker Machine is used to create the swarm nodes using VirtualBox (for macOS) and Hyper-V (for Windows).

## 1. Create swarm nodes

Following bash script is a simpler form of Docker's own swarm beginner tutorial [bash script](https://github.com/docker/labs/blob/master/swarm-mode/beginner-tutorial/swarm-node-vbox-setup.sh). You can use this for Linux and macOS. If you are using Windows, then download and change the [Powershell Hyper-V version](https://github.com/docker/labs/blob/master/swarm-mode/beginner-tutorial/swarm-node-hyperv-setup.ps1) of the same script.

- The script first instantiates three [nodes](https://docs.docker.com/engine/swarm/how-swarm-mode-works/nodes/) using Docker Machine and VirtualBox. Thereafter, it initializes the swarm cluster by creating a swarm [manager](https://docs.docker.com/engine/swarm/how-swarm-mode-works/nodes/#manager-nodes) on the first node. Finally, it adds the remaining nodes as [workers](https://docs.docker.com/engine/swarm/how-swarm-mode-works/nodes/#worker-nodes) to the cluster. It also pulls the `yugabytedb/yugabyte` container image into each of the nodes to expedite the next steps.

{{< note title="Note" >}}
In more fault-tolerant setups, there will be multiple manager nodes and they will be independent of the worker nodes. A 3-node master and 3-node worker setup is used in the Docker tutorial script referenced above.
{{< /note >}}

```sh
#!/bin/bash

# Swarm mode using Docker Machine

workers=3

# create worker machines
echo "======> Creating $workers worker machines ...";
for node in $(seq 1 $workers);
do
    echo "======> Creating worker$node machine ...";
    docker-machine create -d virtualbox worker$node;
done

# list all machines
docker-machine ls

# initialize swarm mode and create a manager on worker1
echo "======> Initializing the swarm manager on worker1 ..."
docker-machine ssh worker1 "docker swarm init --listen-addr $(docker-machine ip worker1) --advertise-addr $(docker-machine ip worker1)"

# get worker tokens
export worker_token=`docker-machine ssh worker1 "docker swarm join-token worker -q"`
echo "worker_token: $worker_token"

# show members of swarm
docker-machine ssh worker1 "docker node ls"

# other workers join swarm, worker1 is already a member
for node in $(seq 2 $workers);
do
    echo "======> worker$node joining swarm as worker ..."
    docker-machine ssh worker$node \
    "docker swarm join \
    --token $worker_token \
    --listen-addr $(docker-machine ip worker$node) \
    --advertise-addr $(docker-machine ip worker$node) \
    $(docker-machine ip worker1)"
done

# pull the yugabytedb container
for node in $(seq 1 $workers);
do
    echo "======> pulling yugabytedb/yugabyte container on worker$node ..."
    docker-machine ssh worker$node \
    "docker pull yugabytedb/yugabyte"
done

# show members of swarm
docker-machine ssh worker1 "docker node ls"
```

- Review all the nodes created.

```sh
$ docker-machine ls
```

```sh
NAME      ACTIVE   DRIVER       STATE     URL                         SWARM   DOCKER        ERRORS
worker1   -        virtualbox   Running   tcp://192.168.99.100:2376           v18.05.0-ce
worker2   -        virtualbox   Running   tcp://192.168.99.101:2376           v18.05.0-ce
worker3   -        virtualbox   Running   tcp://192.168.99.102:2376           v18.05.0-ce
```

## 2. Create overlay network

- SSH into the worker1 node where the swarm manager is running.

```sh
$ docker-machine ssh worker1
```

- Create an [overlay network](https://docs.docker.com/network/overlay/) that the swarm services can use to communicate with each other. The `attachable` option allows standalone containers to connect to swarm services on the network.

```sh
$ docker network create --driver overlay --attachable yugabytedb
```

## 3. Create yb-master services

- Create 3 YB-Master [`replicated`](https://docs.docker.com/engine/swarm/how-swarm-mode-works/services/) services each with replicas set to 1. This is the [only way](https://github.com/moby/moby/issues/30963) in Docker Swarm today to get stable network identities for each of the YB-Master containers that you will need to provide as input for creating the YB-TServer service in the next step.

{{< note title="Note for Kubernetes users" >}}

Docker Swarm lacks an equivalent of [Kubernetes StatefulSets](https://kubernetes.io/docs/concepts/workloads/controllers/statefulset/). The concept of replicated services is similar to [Kubernetes deployments](https://kubernetes.io/docs/concepts/workloads/controllers/deployment/).

{{< /note >}}

```sh
$ docker service create \
--replicas 1 \
--name yb-master1 \
--hostname yb-master1 \
--network yugabytedb \
--mount type=volume,source=yb-master1,target=/mnt/data0 \
--publish 7000:7000 \
yugabytedb/yugabyte:latest /home/yugabyte/bin/yb-master \
--fs_data_dirs=/mnt/data0 \
--master_addresses=yb-master1:7100,yb-master2:7100,yb-master3:7100 \
--rpc_bind_addresses=yb-master1:7100 \
--replication_factor=3
```

```sh
$ docker service create \
--replicas 1 \
--name yb-master2 \
--hostname yb-master2 \
--network yugabytedb \
--mount type=volume,source=yb-master2,target=/mnt/data0 \
yugabytedb/yugabyte:latest /home/yugabyte/bin/yb-master \
--fs_data_dirs=/mnt/data0 \
--master_addresses=yb-master1:7100,yb-master2:7100,yb-master3:7100 \
--rpc_bind_addresses=yb-master2:7100 \
--replication_factor=3
```

```sh
$ docker service create \
--replicas 1 \
--name yb-master3 \
--hostname yb-master3 \
--network yugabytedb \
--mount type=volume,source=yb-master3,target=/mnt/data0 \
yugabytedb/yugabyte:latest /home/yugabyte/bin/yb-master \
--fs_data_dirs=/mnt/data0 \
--master_addresses=yb-master1:7100,yb-master2:7100,yb-master3:7100 \
--rpc_bind_addresses=yb-master3:7100 \
--replication_factor=3
```

- Run the command below to see the services that are now live.

```sh
$ docker service ls
```

```sh
ID                  NAME                MODE                REPLICAS            IMAGE                        PORTS
jfnrqfvnrc5b        yb-master1          replicated          1/1                 yugabytedb/yugabyte:latest   *:7000->7000/tcp
kqp6eju3kq88        yb-master2          replicated          1/1                 yugabytedb/yugabyte:latest
ah6wfodd4noh        yb-master3          replicated          1/1                 yugabytedb/yugabyte:latest
```

- View the yb-master Admin UI by going to the port 7000 of any node, courtesy of the publish option used when yb-master1 was created. For example, you can see from Step 1 that worker2's IP address is `192.168.99.101`. So, `http://192.168.99.101:7000` takes us to the yb-master Admin UI.

## 4. Create yb-tserver service

- Create a single yb-tserver [`global`](https://docs.docker.com/engine/swarm/how-swarm-mode-works/services/) service so that swarm can then automatically spawn 1 container/task on each worker node. Each time you add a node to the swarm, the swarm orchestrator creates a task and the scheduler assigns the task to the new node.

{{< note title="Note for Kubernetes Users" >}}
The global services concept in Docker Swarm is similar to [Kubernetes DaemonSets](https://kubernetes.io/docs/concepts/workloads/controllers/daemonset/).
{{< /note >}}

```sh
$ docker service create \
--mode global \
--name yb-tserver \
--network yugabytedb \
--mount type=volume,source=yb-tserver,target=/mnt/data0 \
--publish 9000:9000 \
yugabytedb/yugabyte:latest /home/yugabyte/bin/yb-tserver \
--fs_data_dirs=/mnt/data0 \
--rpc_bind_addresses=0.0.0.0:9100 \
--tserver_master_addrs=yb-master1:7100,yb-master2:7100,yb-master3:7100
```

{{< tip title="Tip" >}}
Use remote volumes instead of local volumes (used above) when you want to scale-out or scale-in your swarm cluster.
{{< /tip >}}

- Run the command below to see the services that are now live.

```sh
$ docker service ls
```

```sh
ID                  NAME                MODE                REPLICAS            IMAGE                        PORTS
jfnrqfvnrc5b        yb-master1          replicated          1/1                 yugabytedb/yugabyte:latest   *:7000->7000/tcp
kqp6eju3kq88        yb-master2          replicated          1/1                 yugabytedb/yugabyte:latest
ah6wfodd4noh        yb-master3          replicated          1/1                 yugabytedb/yugabyte:latest
n6padh2oqjk7        yb-tserver          global              3/3                 yugabytedb/yugabyte:latest   *:9000->9000/tcp
```

- Now you can go to `http://192.168.99.101:9000` to see the yb-tserver admin UI.

## 5. Test the APIs

### YSQL API

- Connect to the ysqlsh client in yb-tserver.

```sh
$ docker exec -it <ybtserver_container_id> /home/yugabyte/bin/ysqlsh
```

```
...
ysqlsh (11.2-YB-2.0.1.0-b0)
Type "help" for help.

yugabyte=#
```

- Follow the test instructions as noted in [Quick Start](../../../quick-start/explore-ysql/).

### YCQL API

- Find the container ID of the yb-tserver running on `worker1`. Use the first parameter of `docker ps` output.

- Connect to that container using that container ID.

```sh
$ docker exec -it <ybtserver_container_id> /home/yugabyte/bin/ycqlsh
```

```sh
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh>
```

- Follow the test instructions as noted in [Quick Start](../../../api/ycql/quick-start/).

### YEDIS API

- Find the container ID of the yb-master running on worker1. Use the first param of `docker ps` output.

- Initialize the YEDIS API.

```sh
$ docker exec -it <ybmaster_container_id> /home/yugabyte/bin/yb-admin --master_addresses yb-master1:7100,yb-master2:7100,yb-master3:7100 setup_redis_table
```

```sh
I0515 19:54:48.952378    39 client.cc:1208] Created table system_redis.redis of type REDIS_TABLE_TYPE
I0515 19:54:48.953572    39 yb-admin_client.cc:440] Table 'system_redis.redis' created.
```

- Follow the test instructions as noted in [Quick Start](../../../yedis/quick-start/).

## 6. Test fault-tolerance with node failure

Docker Swarm ensures that the yb-tserver `global` service will always have 1 yb-tserver container running on every node. If the yb-tserver container on any node dies, then Docker Swarm will bring it back on.

```sh
$ docker kill <ybtserver_container_id>
```

Observe the output of `docker ps` every few seconds till you see that the yb-tserver container is re-spawned by Docker Swarm.

## 7. Test auto-scaling with node addition

- On the host machine, get worker token for new worker nodes to use to join the existing swarm.

```sh
$ docker-machine ssh worker1 "docker swarm join-token worker -q"
```

```
SWMTKN-1-aadasdsadas-2ja2q2esqsivlfx2ygi8u62yq
```

- Create a new node `worker4`.

```sh
$ docker-machine create -d virtualbox worker4
```

- Pull the YugabyteDB container.

```sh
$ docker-machine ssh worker4 "docker pull yugabytedb/yugabyte"
```

- Join worker4 with existing swarm.

```sh
$ docker-machine ssh worker4 \
    "docker swarm join \
    --token SWMTKN-1-aadasdsadas-2ja2q2esqsivlfx2ygi8u62yq \
    --listen-addr $(docker-machine ip worker4) \
    --advertise-addr $(docker-machine ip worker4) \
    $(docker-machine ip worker1)"
```

- Observe that Docker Swarm adds a new yb-tserver instance to the newly added `worker4` node and changes its replica status from 3 / 3 to 4 / 4.

```sh
$ docker service ls
```

```sh
ID                  NAME                MODE                REPLICAS            IMAGE                        PORTS
jfnrqfvnrc5b        yb-master1          replicated          1/1                 yugabytedb/yugabyte:latest   *:7000->7000/tcp
kqp6eju3kq88        yb-master2          replicated          1/1                 yugabytedb/yugabyte:latest
ah6wfodd4noh        yb-master3          replicated          1/1                 yugabytedb/yugabyte:latest
n6padh2oqjk7        yb-tserver          global              4/4                 yugabytedb/yugabyte:latest   *:9000->9000/tcp
```

## 8. Remove services and destroy nodes

- Stop the machines.

```sh
$ docker-machine stop $(docker-machine ls -q)
```

- Remove the machines.

```sh
$ docker-machine rm $(docker-machine ls -q)
```
