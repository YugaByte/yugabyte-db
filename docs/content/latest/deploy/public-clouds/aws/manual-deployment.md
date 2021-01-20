---
title: Manually deploy on Amazon Web Services
headerTitle: Amazon Web Services
linkTitle: Amazon Web Services
description: Manually deploy a YugabyteDB cluster on Amazon Web Services.
menu:
  latest:
    identifier: deploy-in-aws-3-manual-deployment
    parent: public-clouds
    weight: 630
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/public-clouds/aws/cloudformation" class="nav-link">
      <i class="icon-shell"></i>
      CloudFormation
    </a>
  </li>
  <li >
    <a href="/latest/deploy/public-clouds/aws/terraform" class="nav-link">
      <i class="icon-shell"></i>
      Terraform
    </a>
  </li>
  <li>
    <a href="/latest/deploy/public-clouds/aws/manual-deployment" class="nav-link active">
      <i class="icon-shell"></i>
      Manual deployment
    </a>
  </li>
</ul>

This page documents the manual deployment of YugabyteDB on six AWS EC2 instances with `c5d.4xlarge` as the instance type and CentOS 7 as the instance operating system. The deployment is configured for multiple availability zones (multi-AZ), with three AZs, and has a replication factor (RF) of `3`. The configuration can be easily changed to handle single-AZ as well as multi-region deployments.

## 1. Prerequisites

### Create AWS EC2 instances

Create AWS EC2 instances with the following characteristics.

- Virtual Machines: Spin up 6 (minimum 3 if you are using RF=3). Given that this is a 3-AZ deployment, a multiple of 3 is preferred.

- Operating System: CentOS 7 VMs of above type. You can use Ubuntu as well, but then some of the specific steps in terms of setting up ulimits etc. could be slightly different.

- Ports: Make sure to bring these VMs up in a Security Group where communication between instances is enabled on these ports (and not locked down by security settings). For a listing of these ports, see [Default ports reference](../../../../reference/configuration/default-ports).

We now have 2 VMs each in Availability Zones `us-west-2a`, `us-west-2b`, `us-west-2c` respectively.

### Set environment variables

Now that the six nodes have been prepared, the yb-master process will be run on three of these nodes (because RF=3) and yb-tserver will be run on all six nodes. To learn more about YugabyteDB’s server architecture, see [here](../../../../architecture/concepts/universe/).

These install steps are written in a way that assumes that you will run the install steps from another node from which you can access the above six VMs over `ssh`.

These are some handy environment variables you can set on the node from which you are planning to do the install of the software on the six nodes.

```sh
# Suppose these are the IP addresses of your 6 machines
# (say 2 in each AZ).
export AZ1_NODES="<ip1> <ip2> ..."
export AZ2_NODES="<ip2> <ip2> ..."
export AZ3_NODES="<ip1> <ip2> ..."

# Version of YugabyteDB you plan to install.
export YB_VERSION=2.3.3.0

# Comma separated list of directories available for YB on each node
# In this example, it is just 1. But if you have two then the RHS
# will be something like /mnt/d0,/mnt/d1.
export DATA_DIRS=/mnt/d0

# PEM file used to access the VM/instances over SSH.
# If you are not using pem file based way of connecting to machines,
# you’ll need to replace the “-i $PEM” ssh option in later
# commands in the document appropriately.
export PEM=~/.ssh/yb-dev-aws-2.pem

# We’ll assume this user has sudo access to mount drives that will
# be used as data directories for YugabyteDB, install xfs (or ext4
# or some reasonable file system), update system ulimits etc.
#
# If those steps are done differently and your image already has
# suitable limits and data directories for YugabyteDB to use then
# you may not need to worry about those steps.
export ADMIN_USER=centos

# We need three masters if Replication Factor (RF=3)
# Take one node or the first node from each AZ to run yb-master.
# (For single AZ deployments just take any three nodes as
# masters.)
#
# You don’t need to CHANGE these unless you want to customize.
export MASTER1=\`echo $AZ1_NODES | cut -f1 -d" "\`
export MASTER2=\`echo $AZ2_NODES | cut -f1 -d" "\`
export MASTER3=\`echo $AZ3_NODES | cut -f1 -d" "\`


# Other Environment vars that are simply derived from above ones.
export MASTER_NODES="$MASTER1 $MASTER2 $MASTER3"
export MASTER_RPC_ADDRS="$MASTER1:7100,$MASTER2:7100,$MASTER3:7100"

# yb-tserver will run on all nodes
# You don’t need to change these
export ALL_NODES="$AZ1_NODES $AZ2_NODES $AZ3_NODES"
export TSERVERS=$ALL_NODES

# The binary that you will use
export TAR_FILE=yugabyte-${YB_VERSION}-linux.tar.gz
```

### Prepare data drives

If your AMI already has the needed hooks for mounting the devices as directories in some well defined location OR if are just trying to use a vanilla directory as the data drive for a quick experiment and do not need mounting the additional devices on your AWS volume, you can just use an arbitrary directory (like `/home/$USER/` as your data directory), and YugabyteDB will create a `yb-data` subdirectory there (`/home/$USER/yb-data`) and use that. The steps below are are simply a guide to help use the additional volumes (install a filesystem on those volumes and mount them in some well defined location so that they can be used as data directories by YugabyteDB).

#### Locate drives

On each of those nodes, first locate the SSD devices to be used as the data directories for YugabyteDB to store data on (such as RAFT/txn logs, SSTable files, logs, etc.).

```sh
$ lsblk
```

```
NAME        MAJ:MIN RM   SIZE RO TYPE MOUNTPOINT
nvme0n1     259:1    0    40G  0 disk
└─nvme0n1p1 259:2    0    40G  0 part /
nvme1n1     259:0    0 372.5G  0 disk
```

OR

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip lsblk; \
done
```

Notice that the 370G partition is on `nvme1n1`, but its MOUNTPOINT column is empty - meaning that it has not been mounted. We should prepare this drive for use by putting a reasonable filesystem on it and mounting it in some well defined location.

#### Create file system

Create xfs file system on those devices. The filesystem on the drives do not have to be XFS. It could be ext4 also, for instance. But we have primarily tested with xfs.

You can run this command on each node OR use the sample loop below.

```sh
$ sudo /sbin/mkfs.xfs /dev/nvme1n1 -f
```

OR

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip sudo /sbin/mkfs.xfs /dev/nvme1n1 -f; \
done
```

Verify the file system.

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip sudo /sbin/blkid -o value -s TYPE -c /dev/null /dev/nvme1n1; \
done
```

The above should print “xfs” for each of the nodes or drives.

#### Configure Drives

Add `/etc/fstab` entry to mount the drives on each of the nodes. This example assumes there’s one drive that you will mount at the `/mnt/d0` location.

```sh
for ip in $ALL_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip \
      sudo "echo '/dev/nvme1n1 /mnt/d0 xfs defaults,noatime,nofail,allocsize=4m 0 2' | sudo tee -a /etc/fstab"; \
done
```

Verify that the file has the expected new entries.

```sh
for ip in $ALL_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip cat /etc/fstab | grep xfs; \
done
```

#### Mount Drives

Mount the drive(s) at `/mnt/d0` path. Note that the `/mnt/d0` is just a sample path. You can use a different location if you prefer. These paths become the `--fs_data_dirs` argument to yb-master and yb-tserver processes in later steps.

```sh
for ip in $ALL_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip sudo mkdir /mnt/d0; \
    ssh -i $PEM $ADMIN_USER@$ip sudo mount /mnt/d0; \
    ssh -i $PEM $ADMIN_USER@$ip sudo chmod 777 /mnt/d0; \
done
```

Verify that the drives were mounted and are of expected size.

```sh
for ip in $ALL_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip df -kh | grep mnt; \
done
```

### Verify system configuration

Below is an example of setting up these prerequisites in CentOS 7 or RHEL. For Ubuntu, the specific steps could be slightly different. For details, see [System configuration](../../../manual-deployment/system-config/).

#### Install ntp and other optional packages

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip sudo yum install -y epel-release ntp; \
done
```

OPTIONAL: You can install a few more helpful packages (for tools like `perf`, `iostat`, `netstat`, `links`)

```sh
for ip in $ALL_NODES; do \
     echo =======$ip=======; \
     ssh -i $PEM $ADMIN_USER@$ip sudo yum install -y perf sysstat net-tools links; \
done
```

#### Set ulimits

To ensure proper ulimit settings needed for YugabyteDB, add these lines to `/etc/security/limits.conf` (or appropriate location based on your OS).

```
*       -       core    unlimited
*       -       nofile  1048576
*       -       nproc   12000
```

Sample commands to set the above on all nodes.

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip sudo "echo '*       -       core    unlimited' | sudo tee -a /etc/security/limits.conf"; \
   ssh -i $PEM $ADMIN_USER@$ip sudo "echo '*       -       nofile  1048576' | sudo tee -a /etc/security/limits.conf"; \
   ssh -i $PEM $ADMIN_USER@$ip sudo "echo '*       -       nproc   12000' | sudo tee -a /etc/security/limits.conf"; \
done
```

Make sure the above is not overridden by files in `limits.d` directory. For example, if `20-nproc.conf` file on the nodes has a different value, then update the file as below.

```sh
$ cat /etc/security/limits.d/20-nproc.conf
```

```
*          soft    nproc     12000
root       soft    nproc     unlimited
```

A sample command to set the above on all nodes.

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip sudo "echo '*       -       nproc   12000' | sudo tee -a /etc/security/limits.d/20-nproc.conf"; \
done
```

#### Verify the settings

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip ulimit -n -u -c; \
done
```

The values should be along the lines of:

```
open files                      (-n) 1048576
max user processes              (-u) 12000
core file size          (blocks, -c) unlimited
```

## 2. Install YugabyteDB

Note: The installation need NOT be undertaken by the root or the `ADMIN_USER` (`centos`). In the examples below, however, these commands are run as the `ADMIN_USER`.

Create `yb-software` & `yb-conf` directory in a directory of your choice. In this example, you use the `ADMIN_USER` home directory.

```sh
for ip in $ALL_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip mkdir -p ~/yb-software; \
    ssh -i $PEM $ADMIN_USER@$ip mkdir -p ~/yb-conf; \
done
```

Download the YugabyteDB package, untar and run the post-install script to patch relative paths on all nodes.

```sh
for ip in $ALL_NODES; do \
   echo =======$ip=======; \
   ssh -i $PEM $ADMIN_USER@$ip \
      "cd ~/yb-software; \
       curl -k -o yugabyte-${YB_VERSION}-linux.tar.gz \
         https://downloads.yugabyte.com/yugabyte-${YB_VERSION}-linux.tar.gz"; \
   ssh -i $PEM $ADMIN_USER@$ip \
      "cd ~/yb-software; \
       tar xvfz yugabyte-${YB_VERSION}-linux.tar.gz"; \
   ssh -i $PEM $ADMIN_USER@$ip \
       "cd ~/yb-software/yugabyte-${YB_VERSION}; \
        ./bin/post_install.sh"; \
done
```

Create `~/master` & `~/tserver` directories as symlinks to installed software directory.

Execute the following on master nodes only.

```sh
for ip in $MASTER_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip \
      "ln -s ~/yb-software/yugabyte-${YB_VERSION} ~/master"; \
done
```

Execute the following on all nodes.

```sh
for ip in $ALL_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip \
      "ln -s ~/yb-software/yugabyte-${YB_VERSION} ~/tserver"; \
done
```

The advantage of using symbolic links (symlinks) is that, when you later need to do a rolling software upgrade, you can upgrade YB-Master and YB-TServer servers one at a time by stopping the YB-Master server, switching the link to the new release, and starting the YB-Master server. Then, do the same for YB-TServer servers.

## 3. Prepare YB-Master configuration files

This step prepares the configuration files for the three masters. The configuration files need to, among other things, have the right information to indicate which cloud, region, or availability zone each master is in.

### Create YB-Master1 configuration file

```sh
(MASTER=$MASTER1; CLOUD=aws; REGION=us-west; AZ=us-west-2a; CONFIG_FILE=~/yb-conf/master.conf ;\
  ssh -i $PEM $ADMIN_USER@$MASTER "
    echo --master_addresses=$MASTER_RPC_ADDRS    > $CONFIG_FILE
    echo --fs_data_dirs=$DATA_DIRS              >> $CONFIG_FILE
    echo --rpc_bind_addresses=$MASTER:7100      >> $CONFIG_FILE
    echo --webserver_interface=$MASTER          >> $CONFIG_FILE
    echo --placement_cloud=$CLOUD               >> $CONFIG_FILE
    echo --placement_region=$REGION             >> $CONFIG_FILE
    echo --placement_zone=$AZ                   >> $CONFIG_FILE
"
)
```

### Create YB-Master2 configuration file

```sh
(MASTER=$MASTER2; CLOUD=aws; REGION=us-west; AZ=us-west-2b; CONFIG_FILE=~/yb-conf/master.conf ;\
  ssh -i $PEM $ADMIN_USER@$MASTER "
    echo --master_addresses=$MASTER_RPC_ADDRS    > $CONFIG_FILE
    echo --fs_data_dirs=$DATA_DIRS              >> $CONFIG_FILE
    echo --rpc_bind_addresses=$MASTER:7100      >> $CONFIG_FILE
    echo --webserver_interface=$MASTER          >> $CONFIG_FILE
    echo --placement_cloud=$CLOUD               >> $CONFIG_FILE
    echo --placement_region=$REGION             >> $CONFIG_FILE
    echo --placement_zone=$AZ                   >> $CONFIG_FILE
"
)
```

### Create YB-Master3 configuration file

```sh
(MASTER=$MASTER3; CLOUD=aws; REGION=us-west; AZ=us-west-2c; CONFIG_FILE=~/yb-conf/master.conf ;\
  ssh -i $PEM $ADMIN_USER@$MASTER "
    echo --master_addresses=$MASTER_RPC_ADDRS    > $CONFIG_FILE
    echo --fs_data_dirs=$DATA_DIRS              >> $CONFIG_FILE
    echo --rpc_bind_addresses=$MASTER:7100      >> $CONFIG_FILE
    echo --webserver_interface=$MASTER          >> $CONFIG_FILE
    echo --placement_cloud=$CLOUD               >> $CONFIG_FILE
    echo --placement_region=$REGION             >> $CONFIG_FILE
    echo --placement_zone=$AZ                   >> $CONFIG_FILE
"
)
```

### Verify

Verify that all the configuration variables look right and environment variables were substituted correctly.

```sh
for ip in $MASTER_NODES; do \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip cat ~/yb-conf/master.conf; \
done
```

## 4. Prepare YB-TServer configuration files

### Create config file for AZ1 yb-tserver nodes

```sh
(CLOUD=aws; REGION=us-west; AZ=us-west-2a; CONFIG_FILE=~/yb-conf/tserver.conf; \
 for ip in $AZ1_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip "
      echo --tserver_master_addrs=$MASTER_RPC_ADDRS            > $CONFIG_FILE
      echo --fs_data_dirs=$DATA_DIRS                          >> $CONFIG_FILE
      echo --rpc_bind_addresses=$ip:9100                      >> $CONFIG_FILE
      echo --cql_proxy_bind_address=$ip:9042                  >> $CONFIG_FILE
      echo --redis_proxy_bind_address=$ip:6379                >> $CONFIG_FILE
      echo --webserver_interface=$ip                          >> $CONFIG_FILE
      echo --placement_cloud=$CLOUD                           >> $CONFIG_FILE
      echo --placement_region=$REGION                         >> $CONFIG_FILE
      echo --placement_zone=$AZ                               >> $CONFIG_FILE
      echo --pgsql_proxy_bind_address=$ip:5433                >> $CONFIG_FILE
    "
 done
)
```

### Create configuration file for AZ2 YB-TServer servers

```sh
(CLOUD=aws; REGION=us-west; AZ=us-west-2b; CONFIG_FILE=~/yb-conf/tserver.conf; \
 for ip in $AZ2_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip "
      echo --tserver_master_addrs=$MASTER_RPC_ADDRS            > $CONFIG_FILE
      echo --fs_data_dirs=$DATA_DIRS                          >> $CONFIG_FILE
      echo --rpc_bind_addresses=$ip:9100                      >> $CONFIG_FILE
      echo --cql_proxy_bind_address=$ip:9042                  >> $CONFIG_FILE
      echo --redis_proxy_bind_address=$ip:6379                >> $CONFIG_FILE
      echo --webserver_interface=$ip                          >> $CONFIG_FILE
      echo --placement_cloud=$CLOUD                           >> $CONFIG_FILE
      echo --placement_region=$REGION                         >> $CONFIG_FILE
      echo --placement_zone=$AZ                               >> $CONFIG_FILE
      echo --pgsql_proxy_bind_address=$ip:5433                >> $CONFIG_FILE
    "
 done
)
```

### Create configuration file for AZ3 YB-TServer servers

```sh
(CLOUD=aws; REGION=us-west; AZ=us-west-2c; CONFIG_FILE=~/yb-conf/tserver.conf; \
 for ip in $AZ3_NODES; do \
    echo =======$ip=======; \
    ssh -i $PEM $ADMIN_USER@$ip "
      echo --tserver_master_addrs=$MASTER_RPC_ADDRS            > $CONFIG_FILE
      echo --fs_data_dirs=$DATA_DIRS                          >> $CONFIG_FILE
      echo --rpc_bind_addresses=$ip:9100                      >> $CONFIG_FILE
      echo --cql_proxy_bind_address=$ip:9042                  >> $CONFIG_FILE
      echo --redis_proxy_bind_address=$ip:6379                >> $CONFIG_FILE
      echo --webserver_interface=$ip                          >> $CONFIG_FILE
      echo --placement_cloud=$CLOUD                           >> $CONFIG_FILE
      echo --placement_region=$REGION                         >> $CONFIG_FILE
      echo --placement_zone=$AZ                               >> $CONFIG_FILE
      echo --pgsql_proxy_bind_address=$ip:5433                >> $CONFIG_FILE
    "
 done
)
```

### Verify

Verify that all the configuration flags look correct and environment variables were substituted correctly.

```sh
for ip in $ALL_NODES; do \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip cat ~/yb-conf/tserver.conf; \
done
```

## 5. Start YB-Master servers

Note: On the first time that all three YB-Master servers are started, it creates the cluster. If a YB-Master server is restarted (after cluster has been created), such as during a rolling upgrade of software, it simply rejoins the cluster.

```sh
for ip in $MASTER_NODES; do \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip \
    "~/master/bin/yb-master --flagfile ~/yb-conf/master.conf \
      >& /mnt/d0/yb-master.out &"; \
done
```

### Verify

Verify that the YB-Master servers are running.

```sh
for ip in $MASTER_NODES; do  \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip ps auxww | grep yb-master; \
done
```

Check the YB-Master UI by going to any of the 3 YB-Master servers.

```
http://<any-master-ip>:7000/
```

You can do so using a character mode browser (such as links for example). Try the following.

```sh
$ links http://<a-master-ip>:7000/
```

### Troubleshooting

Make sure all the ports detailed in the earlier section are opened up. Else, check the log at `/mnt/d0/yb-master.out` for `stdout` or `stderr` output from the YB-Master server. Also, check INFO/WARNING/ERROR/FATAL glogs output by the process in the `/mnt/d0/yb-data/master/logs/*`

## 6. Start YB-TServer servers

After starting all the YB-Master servers in the previous step, start YB-TServer servers on all the nodes.

```sh
for ip in $ALL_NODES; do \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip \
    "~/tserver/bin/yb-tserver --flagfile ~/yb-conf/tserver.conf \
      >& /mnt/d0/yb-tserver.out &"; \
done
```

Verify that the YB-TServer servers are running.

```sh
for ip in $ALL_NODES; do  \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip ps auxww | grep yb-tserver; \
done
```

## 7. Configure AZ- and region-aware placement

Note: This example is a multi-AZ (single region deployment). 

The default replica placement policy when the cluster is first created is to treat all nodes as equal irrespective of the placement_* configuration flags. However, for the current deployment, you want to explicitly place 1 replica in each AZ. The following command sets replication factor of 3 across `us-west-2a`, `us-west-2b` and `us-west-2c` leading to the placement of 1 replica in each AZ.

```sh
ssh -i $PEM $ADMIN_USER@$MASTER1 \
   ~/master/bin/yb-admin --master_addresses $MASTER_RPC_ADDRS \
    modify_placement_info  \
    aws.us-west.us-west-2a,aws.us-west.us-west-2b,aws.us-west.us-west-2c 3
```

Verify by running the following.

```sh
$ curl -s http://<any-master-ip>:7000/cluster-config
```

And confirm that the output looks similar to what is shown below with `min_num_replicas` set to 1 for each AZ.

```
replication_info {
  live_replicas {
    num_replicas: 3
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2a"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2b"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2c"
      }
      min_num_replicas: 1
    }
  }
}
```

Suppose your deployment is multi-region rather than multi-zone, one additional option to consider is to set a preferred location for all the tablet leaders using the [yb-admin set_preferred_zones](../../../../admin/yb-admin/#set-preferred-zones) command. For multi-row or multi-table transactional operations, colocating the leaders to be in a single zone/region can help reduce the number of cross-region network hops involved in executing the transaction and as a result improve performance.

The following command sets the preferred zone to `aws.us-west.us-west-2c`:

```sh
ssh -i $PEM $ADMIN_USER@$MASTER1 \
   ~/master/bin/yb-admin --master_addresses $MASTER_RPC_ADDRS \
    set_preferred_zones  \
    aws.us-west.us-west-2c
```

Looking again at the cluster config you should see `affinitized_leaders` added:

```
replication_info {
  live_replicas {
    num_replicas: 3
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2a"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2b"
      }
      min_num_replicas: 1
    }
    placement_blocks {
      cloud_info {
        placement_cloud: "aws"
        placement_region: "us-west"
        placement_zone: "us-west-2c"
      }
      min_num_replicas: 1
    }
  }
  affinitized_leaders {
    placement_cloud: "aws"
    placement_region: "us-west"
    placement_zone: "us-west-2c"
  }
}
```

## 8. Test PostgreSQL-compatible YSQL API

Connect to the cluster using the YSQL shell (`ysqlsh`) that is installed in the `bin` directory.
If you want to use `ysqlsh` from a different node, follow the steps on the [ysqlsh](../../../../admin/ysqlsh/) page.

From any node, execute the following command.

```sh
$ cd ~/tserver
$ ./bin/ysqlsh <any-node-ip>
```

```sql
CREATE DATABASE yb_test;

\connect yb_test;

CREATE TABLE yb_table(id bigserial PRIMARY KEY);

INSERT INTO yb_table(id) VALUES (1);
INSERT INTO yb_table(id) VALUES (2);
INSERT INTO yb_table(id) VALUES (3);

SELECT * FROM yb_table;
```

Output should be the following:

```sql
 id 
----
  3
  2
  1
(3 rows)
```

## 9. Test Cassandra-compatible YCQL API

### Using ycqlsh

Connect to the cluster using the YCQL shell (`ycqlsh`) that comes installed in the `bin` directory. If you want to use `ycqlsh` from a different node, follow the steps found on the [ycqlsh](../../../../admin/cqlsh/) page.

From any node, execute the following command.

```sh
$ cd ~/tserver
$ ./bin/ycqlsh <any-node-ip>
```

```sql
CREATE KEYSPACE IF NOT EXISTS app;

USE app;

DROP TABLE IF EXISTS user_actions;

CREATE TABLE user_actions (userid int, action_id int, payload text,
        PRIMARY KEY ((userid), action_id))
        WITH CLUSTERING ORDER BY (action_id DESC);

INSERT INTO user_actions (userid, action_id, payload) VALUES (1, 1, 'a');
INSERT INTO user_actions (userid, action_id, payload) VALUES (1, 2, 'b');
INSERT INTO user_actions (userid, action_id, payload) VALUES (1, 3, 'c');
INSERT INTO user_actions (userid, action_id, payload) VALUES (1, 4, 'd');
INSERT INTO user_actions (userid, action_id, payload) VALUES (2, 1, 'l');
INSERT INTO user_actions (userid, action_id, payload) VALUES (2, 2, 'm');
INSERT INTO user_actions (userid, action_id, payload) VALUES (2, 3, 'n');
INSERT INTO user_actions (userid, action_id, payload) VALUES (2, 4, 'o');
INSERT INTO user_actions (userid, action_id, payload) VALUES (2, 5, 'p');

SELECT * FROM user_actions WHERE userid=1 AND action_id > 2;
```

Output should be the following.

```
 userid | action_id | payload
--------+-----------+---------
      1 |         4 |       d
      1 |         3 |       c
```

### Running sample workload

If you want to try the pre-bundled `yb-sample-apps.jar` for some sample apps, you can either use a separate load tester machine (recommended) or use one of the nodes itself.

First install Java on the machine.

```sh
$ sudo yum install java-1.8.0-openjdk-src.x86_64 -y
```

Set the environment variable for the YCQL endpoint.

```sh
$ export CIP_ADDR=<one-node-ip>:9042
```

Here's how to run a workload with 100M key values.

```
% cd ~/tserver/java
% java -jar yb-sample-apps.jar --workload CassandraKeyValue --nodes $CIP_ADDR -num_threads_read 4 -num_threads_write 32 --num_unique_keys 100000000 --num_writes 100000000 --nouuid
```

Here's how to run a workload with 100M records with a unique secondary index.

```
% cd ~/tserver/java
% java -jar yb-sample-apps.jar --workload CassandraUniqueSecondaryIndex --nodes $CIP_ADDR -num_threads_read 1 -num_threads_write 16 --num_unique_keys 100000000 --num_writes 100000000 --nouuid
```

When workload is running, verify activity across various tablet-servers in the Master’s UI:

```
http://<master-ip>:7000/tablet-servers
```

When workload is running, verify active YCQL or YEDIS RPC calls from this links on the “utilz” page.

```
http://<any-tserver-ip>:9000/utilz
```

## 10. Test Redis-compatible YEDIS API

### Prerequisite

Create the YugabyteDB `system_redis.redis` (which is the default Redis database `0`) table using `yb-admin` or using `redis-cli`.

- Using `yb-admin`

```sh
$ cd ~/tserver
$ ./bin/yb-admin --master_addresses $MASTER_RPC_ADDRS setup_redis_table
```

- Using `redis-cli` (which comes pre-bundled in the `bin` directory)

```sh
$ cd ~/tserver
$ ./bin/redis-cli -h <any-node-ip>
```

```sql
> CREATEDB 0
```

### Test API

```sh
$ ./bin/redis-cli -h <any-node-ip>
```

```sql
> SET key1 hello_world
> GET key1
```

## 11. Stop cluster and delete data

The following commands can be used to stop the cluster as well as delete the data directories.

```sh
for ip in $ALL_NODES; do \
  echo =======$ip=======; \
  ssh -i $PEM $ADMIN_USER@$ip pkill yb-master; \
  ssh -i $PEM $ADMIN_USER@$ip pkill yb-tserver; \
  # This assumes /mnt/d0 was the only data dir used on each node. \
  ssh -i $PEM $ADMIN_USER@$ip rm -rf /mnt/d0/yb-data/*; \
done
```
