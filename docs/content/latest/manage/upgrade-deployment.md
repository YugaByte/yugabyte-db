---
title: Upgrade a deployment
headerTitle: Upgrade a deployment
linkTitle: Upgrade a deployment
description: Upgrade a deployment
aliases:
  - /deploy/manual-deployment/upgrade-deployment
  - /latest/manage/upgrade-deployment
menu:
  latest:
    identifier: manage-upgrade-deployment
    parent: manage
    weight: 706
isTocNested: true
showAsideToc: true
---

The basic flow is to upgrade each YB-Master and YB-TServer one at a time, verifying after each step from the yb-master Admin UI that the cluster is healthy and the upgraded process is back online.

If you plan to script this in a loop, then a pause of about 60 seconds is recommended before moving from one process or node to another.

{{<tip title="Preserving data and cluster configuration across upgrades" >}}

Your data/log/conf directories are generally stored in a separate location which stays the same across the upgrade so that the cluster data, its configuration settings are retained across the upgrade.

{{< /tip >}}

## Install new version of YugabyteDB

First you need to install the new version of YugabyteDB in a new location. 
For CentOS, this would be something like:

```
1. wget https://downloads.yugabyte.com/yugabyte-$VER.tar.gz
2. tar xf yugabyte-$VER.tar.gz -C /home/yugabyte/softwareyb-$VER/ 
3. cd /home/yugabyte/softwareyb-$VER/
4. ./bin/post_install.sh
```


{{< note title="Note" >}}

If you are using PostgreSQL extensions, make sure to install the extensions in the new YugabyteDB version before upgrading. Follow the steps in [Install and use extensions](../../api/ysql/extensions). 

{{< /note >}}

## Upgrade YB-Masters

```
1. pkill yb-master  (i.e. stop the older version of the yb-master process)
2. make sure we're on the dir of the new version (cd /home/yugabyte/softwareyb-$VER/) 
3. start  (the newer version of) the yb-master process
4. verify in http://<any-yb-master>:7000/ that all masters are alive
5. pause ~60 secs before upgrading next yb-master
```

## Upgrade YB-TServers

```
1. pkill yb-tserver (i.e. stop the older version of the yb-tserver process)
2. make sure we're on the dir of the new version (cd /home/yugabyte/softwareyb-$VER/) 
3. start  (the newer version of) yb-tserver process
4. verify in http://<any-yb-master>:7000/tablet-servers to see if the new YB-TServer is alive and heart beating
5. pause ~60 secs before upgrading next YB-TServer
```
