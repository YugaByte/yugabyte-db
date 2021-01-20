---
title: Migrate from Helm 2 to Helm 3
headerTitle: Migrate from Helm 2 to Helm 3
linkTitle: Migrate to Helm 3
description: Migrate your YugabyteDB universes and Yugabyte Platform from Helm 2 to Helm 3.
aliases:
  - /latest/manage/enterprise-edition/migrate-to-helm3/
menu:
  latest:
    identifier: migrate-to-helm3
    parent: manage-deployments
    weight: 90
isTocNested: true
showAsideToc: true
---


{{< note title="Note" >}}

Starting with YugabyteDB version 2.1.8, Helm 2 is not supported. Follow the steps below to migrate your existing YugabyteDB universes and the Yugabyte Platform from Helm 2 to Helm 3.

{{< /note >}}

## Migrate existing Yugabyte Platform and YugabyteDB from Helm 2 to Helm 3

1. Check the chart name using the `helm2 ls` command.

```sh
$ helm2 ls
```

```
NAME   	REVISION	UPDATED                 STATUS  	CHART         	APP VERSION	NAMESPACE
yw-test	1       	Tue May 12 22:21:16 2020	DEPLOYED	yugaware-2.2.0 2.2.0.0-76 	yw-test  
```

2. Migrate the chart to Helm 3 using the `2to3` plugin by running the following command.

For details, see [Migrating Helm v2 to v3](https://helm.sh/docs/topics/v2_v3_migration/)].

```sh
$ helm 2to3 convert yw-test
```

```
2020/05/12 22:25:49 Release "yw-test" will be converted from Helm v2 to Helm v3.
2020/05/12 22:25:49 [Helm 3] Release "yw-test" will be created.
2020/05/12 22:25:49 [Helm 3] ReleaseVersion "yw-test.v1" will be created.
2020/05/12 22:25:49 [Helm 3] ReleaseVersion "yw-test.v1" created.
2020/05/12 22:25:49 [Helm 3] Release "yw-test" created.
2020/05/12 22:25:49 Release "yw-test" was converted successfully from Helm v2 to Helm v3.
2020/05/12 22:25:49 Note: The v2 release information still remains and should be removed to avoid conflicts with the migrated v3 release.
2020/05/12 22:25:49 v2 release information should only be removed using `helm 2to3` cleanup and when all releases have been migrated over.
```

3. Verify that the chart has been migrated to Helm 3 by running the following command.

```sh
$ helm ls -n yw-test
```

```
NAME   	NAMESPACE	REVISION	UPDATED                               	STATUS  	CHART         	APP VERSION
yw-test	yw-test  	1       	2020-06-16 16:51:16.44463488 +0000 UTC	deployed	yugaware-2.2.0	2.2.0.0-b80 
```

## Upgrade Yugabyte Platform and YugabyteDB using Helm 3

Because older charts (v.2.1.4 and earlier) used to have the `clusterIP` field in the `services.yaml` file, and the Helm 3 binary doesn’t support leaving the `clusterIP` field empty [[https://github.com/helm/helm/issues/6378](https://github.com/helm/helm/issues/6378)], you need to manually specify the `clusterIP`. You can get the `clusterIP` of the service using the following command:

```sh
$ kubectl get svc -n yw-test
```

```
NAME                         TYPE                CLUSTER-IP    EXTERNAL-IP   PORT(S)                                       AGE
yw-test-yugaware-ui   LoadBalancer   10.103.85.235   10.103.85.235   80:30265/TCP,9090:30661/TCP   18m
```

Also, because the `Release.Service` for Helm 3 is`Helm`, and Helm 2 had it as `Tiller`, any deployments done with Helm 2 will have the `heritage` field set to `Tiller`.  To explicitly set the heritage value to `Tiller`, you can set the `helm2Legacy` variable to  `true`.

For Yugabyte Platform, set the following fields, along with the other necessary changes, by running the following command.

```sh
22:39 $ helm upgrade yw-test yugaware --set helm2Legacy=true,yugaware.service.clusterIP="10.103.85.235" -n yw-test
```

```
Release "yw-test" has been upgraded. Happy Helming!
NAME: yw-test
LAST DEPLOYED: Tue May 12 22:43:49 2020
NAMESPACE: yw-test
STATUS: deployed
REVISION: 2
TEST SUITE: None
```

Omit the `helm2Legacy` field if the chart was not migrated from Helm 2.

For YugabyteDB, create an overrides file, including the following:

```
serviceEndpoints:
  - name: "yb-master-ui"
    type: LoadBalancer
    app: "yb-master"
    ports:
      ui: "7000"
    clusterIP: "<clusterIP of yb-master-ui>"

  - name: "yb-tserver-service"
    type: LoadBalancer
    app: "yb-tserver"
    ports:
      yql-port: "9042"
      yedis-port: "6379"
      ysql-port: "5433"
    clusterIP: "<clusterIP of yb-tserver-service>"

# If the original deployment was done using helm2
helm2Legacy: true

# Whatever other fields need to be modified/updated.
```

Now, run the following command:

```sh
$ helm upgrade yb-test yugabyte/ -f ~/Desktop/test.yaml -n yb-test
```

You should see the following message.

```
Release "yb-test" has been upgraded. Happy Helming!
NAME: yb-test
LAST DEPLOYED: Wed May 13 00:00:01 2020
NAMESPACE: yb-test
STATUS: deployed
REVISION: 3
TEST SUITE: None
NOTES:
1. Get YugabyteDB Pods by running this command:
  kubectl --namespace yb-test get pods
2. Get list of YugabyteDB services that are running:
  kubectl --namespace yb-test get services
3. Get information about the load balancer services:
  kubectl get svc --namespace yb-test
4. Connect to one of the tablet server:
  kubectl exec --namespace yb-test -it yb-tserver-0 -- bash
5. Run YSQL shell from inside of a tablet server:
  kubectl exec --namespace yb-test -it yb-tserver-0 -- ysqlsh -h yb-tserver-0.yb-tservers.yb-test
6. Cleanup YugabyteDB Pods
  For helm 2:
  helm delete yb-test --purge
  For helm 3:
  helm delete yb-test -n yb-test
  NOTE: You need to manually delete the persistent volume
  kubectl delete pvc --namespace yb-test -l app=yb-master
  kubectl delete pvc --namespace yb-test -l app=yb-tserver
```

## Migrate a YugabyteDB universe deployed using Yugabyte Platform

All YugabyteDB clusters deployed on Kubernetes with the Yugabyte Platform before version 2.1.8 were deployed using Helm 2. Starting with YubabyteDB version 2.1.8, the Yugabyte Platform only supports Helm 3, so you need to manually migrate the older YugabyteDB deployments using the `2to3` plugin. By default, any older YugabyteDB on Kubernetes will not be modifiable using the Yugabyte Platform unless it is ported to Helm 3. This can be done as follows:

1. Get all older Helm 2 release names by running the following command.

```sh
$ helm2 ls
NAME              REVISION  UPDATED                 	       STATUS  	CHART                  APP VERSION	NAMESPACE      
yb-admin-test-a	1       Tue May 12 23:08:30 2020    DEPLOYED	yugabyte-2.1.2       2.1.2.0-b10	yb-admin-test-a
yb-admin-test-b	1       Tue May 12 23:08:30 2020    DEPLOYED	yugabyte-2.1.2       2.1.2.0-b10	yb-admin-test-b
yb-admin-test-c	1       Tue May 12 23:08:30 2020    DEPLOYED	yugabyte-2.1.2       2.1.2.0-b10	yb-admin-test-c
```


2. Migrate all releases using the `2to3` plugin by running the following command.

```sh
$ helm 2to3 convert yb-admin-test-a
```

For each migration, you will see messages like this:

```
2020/05/12 23:20:42 Release "yb-admin-test-a" will be converted from Helm v2 to Helm v3.
2020/05/12 23:20:42 [Helm 3] Release "yb-admin-test-a" will be created.
2020/05/12 23:20:42 [Helm 3] ReleaseVersion "yb-admin-test-a.v1" will be created.
2020/05/12 23:20:42 [Helm 3] ReleaseVersion "yb-admin-test-a.v1" created.
2020/05/12 23:20:42 [Helm 3] Release "yb-admin-test-a" created.
2020/05/12 23
:20:42 Release "yb-admin-test-a" was converted successfully from Helm v2 to Helm v3.
2020/05/12 23:20:42 Note: The v2 release information still remains and should be removed to avoid conflicts with the migrated v3 release.
2020/05/12 23:20:42 v2 release information should only be removed using `helm 2to3` cleanup and when all releases have been migrated over.
```

Next, run the same command for the other releases.

```sh
$ helm 2to3 convert yb-admin-test-b
```

```sh
$ helm 2to3 convert yb-admin-test-c
```

3. Verify that the migrations were successful by running the following command.

```sh
$ helm ls -A
NAME           	NAMESPACE     REVISION   UPDATED                 STATUS    CHART         	APP VERSION
yb-admin-test-a	yb-admin-test-a	1       	2020-05-12	deployed	  yugabyte-2.1.2	2.1.2.0-b10
yb-admin-test-b	yb-admin-test-b	1       	2020-05-12	deployed	  yugabyte-2.1.2	2.1.2.0-b10
yb-admin-test-c	yb-admin-test-c	1       	2020-05-12	deployed	  yugabyte-2.1.2	2.1.2.0-b10
```

4. Make the following API call for each universe that has been migrated:

```sh
$ curl --location --request PUT 'http://localhost:9000/api/v1/customers/f33e3c9b-75ab-4c30-80ad-cba85646ea39/universes/d565bf24-39d0-4a90-a9d9-a2441e48a28e/mark_helm3_compatible' \
--header 'X-AUTH-TOKEN: 4832fe2e-13cd-4aa6-9519-a980f14aeee2' \
--header 'Content-Type: application/json' \
--header 'Content-Type: text/plain' \
--data-raw '{}'
```

You've completed the migrations and now the YugabyteDB universe operations can be performed using Yugabyte Platform v.2.1.8 or later.

## Delete old YugabyteDB universes from Yugabyte Platform

Yugabyte Platform will allow deletion of older YugabyteDB universes, but  it will not delete the Helm 2 release. Yugabyte Platform  can also delete the namespaces and associated resources.
