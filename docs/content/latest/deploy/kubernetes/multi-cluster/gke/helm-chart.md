---
title: Deploy a multi-region cluster on Google Kubernetes Engine (GKE) using Helm Chart
headerTitle: Google Kubernetes Engine (GKE)
linkTitle: Google Kubernetes Engine (GKE)
description: Use Helm Chart to deploy a multi-region YugabyteDB cluster that spans 3 GKE clusters across 3 regions.
menu:
  latest:
    parent: deploy-kubernetes-mc
    name: Google Kubernetes Engine
    identifier: k8s-mc-gke-1
    weight: 628
aliases:
  - /latest/deploy/kubernetes/multi-cluster/gke
  - /latest/deploy/kubernetes/multi-cluster/gke/helm-chart
type: page
isTocNested: true
showAsideToc: true
---

<ul class="nav nav-tabs-alt nav-tabs-yb">
  <li >
    <a href="/latest/deploy/kubernetes/multi-cluster/gke/helm-chart" class="nav-link active">
      <i class="fas fa-cubes" aria-hidden="true"></i>
      Helm chart
    </a>
  </li>
</ul>

Following instructions highlight how to deploy a single multi-region YugabyteDB cluster that spans three [GKE](https://cloud.google.com/kubernetes-engine/docs/) clusters, each running in a different region. Each region also has an internal DNS load balancer set to [global access](https://cloud.google.com/kubernetes-engine/docs/how-to/internal-load-balancing#global_access). This configuration allows pods in one GKE cluster to discover pods in another GKE cluster without exposing any of the DNS information to the world outside your GKE project.

We will use the standard single-zone YugabyteDB Helm Chart to deploy one third of the nodes in the database cluster in each of the 3 GKE clusters. 

## Prerequisites

You must have 3 GKE clusters with Helm configured. If you have not installed the Helm client (`helm`), see [Installing Helm](https://helm.sh/docs/intro/install/).

The YugabyteDB Helm chart has been tested with the following software versions:

- GKE running Kubernetes 1.14 or later with nodes such that a total of 12 CPU cores and 45 GB RAM can be allocated to YugabyteDB. This can be three nodes with 4 CPU core and 15 GB RAM allocated to YugabyteDB. `n1-standard-8` is the minimum instance type that meets these criteria.
- Helm 3.0 or later
- YugabyteDB Docker image (yugabytedb/yugabyte) 2.1.0 or later
- For optimal performance, ensure you've set the appropriate [system limits using `ulimit`](../../../../manual-deployment/system-config/#ulimits) on each node in your Kubernetes cluster.

The following steps show how to meet these prerequisites.

- Download and install the [Google Cloud SDK](https://cloud.google.com/sdk/downloads/).

- Configure defaults for gcloud

Set the project ID as `yugabyte`. You can change this as per your need.

```sh
$ gcloud config set project yugabyte
```

- Install `kubectl`

After installing Cloud SDK, install the `kubectl` command line tool by running the following command. 

```sh
$ gcloud components install kubectl
```

Note that GKE is usually 2 or 3 major releases behind the upstream/OSS Kubernetes release. This means you have to make sure that you have the latest `kubectl` version that is compatible across different Kubernetes distributions if that's what you intend to.

- Ensure `helm` is installed

First, check to see if Helm is installed by using the Helm version command.

```sh
$ helm version
```

For Helm 3, you should see something similar to the following output. Note that the `tiller` server side component has been removed in Helm 3.

```
version.BuildInfo{Version:"v3.0.3", GitCommit:"ac925eb7279f4a6955df663a0128044a8a6b7593", GitTreeState:"clean", GoVersion:"go1.13.6"}
```

## 1. Create GKE clusters

### Create clusters

Following commands create 3 Kubernetes clusters in 3 different regions (`us-west1`,`us-central1`,`us-east1`), with 1 node in each cluster.

- This example highlights a multi-region Kubernetes configuration along with a multi-cluster Kubernetes configuration.

- [Global access](https://cloud.google.com/kubernetes-engine/docs/how-to/internal-load-balancing#global_access) on load balancers is currently a beta feature and is available only on GKE clusters created using the `rapid` release channel.

```sh
$ gcloud beta container clusters create yugabytedb1 \
     --machine-type=n1-standard-8 \
     --num-nodes 1 \
     --zone us-west1-b \
     --release-channel rapid
```

```sh
$ gcloud beta container clusters create yugabytedb2 \
     --machine-type=n1-standard-8 \
     --num-nodes 1 \
     --zone us-central1-b \
     --release-channel rapid
```

```sh
$ gcloud beta container clusters create yugabytedb3 \
     --machine-type=n1-standard-8 \
     --num-nodes 1 \
     --zone us-east1-b \
     --release-channel rapid
```

Confirm that you now have 3 Kubernetes contexts as shown below.

```sh
kubectl config get-contexts
```

```
CURRENT   NAME                                          CLUSTER                                 ...                                  
          gke_yugabyte_us-central1-b_yugabytedb2        gke_yugabyte_us-central1-b_yugabytedb2                
*         gke_yugabyte_us-east1-b_yugabytedb3           gke_yugabyte_us-east1-b_yugabytedb3                            
          gke_yugabyte_us-west1-b_yugabytedb1           gke_yugabyte_us-west1-b_yugabytedb1                       
```

### Create a storage class per zone

We need to ensure that the storage classes used by the pods in a given zone are always pinned to that zone only.

Copy the contents below to a file named `gke-us-west1-b.yaml`.

```sh
kind: StorageClass
apiVersion: storage.k8s.io/v1
metadata:
  name: standard-us-west1-b
provisioner: kubernetes.io/gce-pd
parameters:
  type: pd-standard
  replication-type: none
  zone: us-west1-b
```

Copy the contents below to a file named `gke-us-central1-b.yaml`.

```sh
kind: StorageClass
apiVersion: storage.k8s.io/v1
metadata:
  name: standard-us-central1-b
provisioner: kubernetes.io/gce-pd
parameters:
  type: pd-standard
  replication-type: none
  zone: us-central1-b
```

Copy the contents below to a file named `gke-us-east1-b.yaml`.

```sh
kind: StorageClass
apiVersion: storage.k8s.io/v1
metadata:
  name: standard-us-east1-b
provisioner: kubernetes.io/gce-pd
parameters:
  type: pd-standard
  replication-type: none
  zone: us-east1-b

```

Apply the above configuration to your clusters.

```sh
kubectl apply -f gke-us-west1-b.yaml --context gke_yugabyte_us-west1-b_yugabytedb1
```

```sh
kubectl apply -f gke-us-central1-b.yaml --context gke_yugabyte_us-central1-b_yugabytedb2
```

```sh
kubectl apply -f gke-us-east1-b.yaml --context gke_yugabyte_us-east1-b_yugabytedb3
```

## 2. Setup global DNS

Now you will set up a global DNS system across all the 3 GKE clusters so that pods in one cluster can connect to pods in another cluster.

### Create load balancer configuration for kube-dns

The YAML file shown below adds an internal load balancer (which is not exposed outside its own Google Cloud region) to Kubernetes's built-in `kube-dns` deployment. By default, the `kube-dns` deployment is accessed only by a `ClusterIP` and not a load balancer. Additionally, allow this load balancer to be [globally accessible](https://cloud.google.com/kubernetes-engine/docs/how-to/internal-load-balancing#global_access) so that each such load balancer is now visible to the 2 other load balancers in the other 2 regions. Note that using external load balancers for this purpose is possible but is not recommended from a security best practices standpoint. This is because the DNS information for all the clusters would now be available for access on the public Internet.

Copy the contents to a file named `yb-dns-lb.yaml`.

```sh
apiVersion: v1
kind: Service
metadata:
  annotations:
    cloud.google.com/load-balancer-type: "Internal"
    networking.gke.io/internal-load-balancer-allow-global-access: "true"
  labels:
    k8s-app: kube-dns
  name: kube-dns-lb
  namespace: kube-system
spec:
  ports:
  - name: dns
    port: 53
    protocol: UDP
    targetPort: 53
  selector:
    k8s-app: kube-dns
  sessionAffinity: None
  type: LoadBalancer
 ```

### Apply the configuration to every cluster

Download the `yb-multiregion-k8s-setup.py` script that will help you automate the setup of the load balancers.

```sh
wget https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/cloud/kubernetes/yb-multiregion-k8s-setup.py
```

The script starts out by creating a new namespace in each of the 3 clusters. Thereafter, it creates 3 internal load balancers for `kube-dns` in the 3 clusters. After the load balancers are created, it configures them using Kubernetes ConfigMap in such a way that they forward DNS requests for zone-scoped namespaces to the relevant Kubernetes cluster's DNS server. Finally it deletes the `kube-dns` pods so that Kubernetes can bring them back up automatically with the new configuration.

Open the script and edit the `contexts` and `regions` sections to reflect your own configuration.

```
# Replace the following with your own k8s cluster contexts
contexts = {
    'us-west1-b': 'gke_yugabyte_us-west1-b_yugabytedb1',
    'us-central1-b': 'gke_yugabyte_us-central1-b_yugabytedb2',
    'us-east1-b': 'gke_yugabyte_us-east1-b_yugabytedb3',
}

# Replace the following with your own `zone`: `region` names
regions = {
    'us-west1-b': 'us-west1',
    'us-central1-b': 'us-central1',
    'us-east1-b': 'us-east1',
}
```

Now run the script as shown below.

```sh
python yb-multiregion-k8s-setup.py
```

```
namespace/yb-demo-us-east1-b created
service/kube-dns-lb created
namespace/yb-demo-us-central1-b created
service/kube-dns-lb created
namespace/yb-demo-us-west1-b created
service/kube-dns-lb created
DNS endpoint for zone us-east1-b: 10.142.15.197
DNS endpoint for zone us-central1-b: 10.128.15.215
DNS endpoint for zone us-west1-b: 10.138.15.237
pod "kube-dns-68b499d58-wn5zv" deleted
pod "kube-dns-68b499d58-h2m28" deleted
pod "kube-dns-68b499d58-4jl89" deleted
```

We now have 3 GKE clusters that essentially have a global DNS service as long as services use zone-scoped namespaces to access each other.

## 3. Create a YugabyteDB cluster

### Add charts repository

To add the YugabyteDB charts repository, run the following command.

```sh
$ helm repo add yugabytedb https://charts.yugabyte.com
```

Make sure that you have the latest updates to the repository by running the following command.

```sh
$ helm repo update
```

Validate that you have the updated chart version.

```sh
$ helm search repo yugabytedb/yugabyte
```

```sh
NAME                CHART VERSION APP VERSION   DESCRIPTION                                       
yugabytedb/yugabyte 2.1.0        2.1.0.0-b18    YugabyteDB is the high-performance distr...
```

### Create override files

Copy the contents below to a file named `overrides-us-west1-b.yaml`.

```sh
isMultiAz: True

AZ: us-west1-b

masterAddresses: "yb-master-0.yb-masters.yb-demo-us-west1-b.svc.cluster.local:7100, yb-master-0.yb-masters.yb-demo-us-central1-b.svc.cluster.local:7100, yb-master-0.yb-masters.yb-demo-us-east1-b.svc.cluster.local:7100"

storage:
  master:
    storageClass: "standard-us-west1-b"
  tserver:
    storageClass: "standard-us-west1-b"

replicas:
  master: 1
  tserver: 1
  totalMasters: 3

gflags:
  master:
    placement_cloud: "gke"
    placement_region: "us-west1"
    placement_zone: "us-west1-b"
    leader_failure_max_missed_heartbeat_periods: 10
  tserver:
    placement_cloud: "gke"
    placement_region: "us-west1"
    placement_zone: "us-west1-b"
    leader_failure_max_missed_heartbeat_periods: 10
```

Copy the contents below to a file named `overrides-us-central1-b.yaml`.

```sh
isMultiAz: True

AZ: us-central1-b

masterAddresses: "yb-master-0.yb-masters.yb-demo-us-west1-b.svc.cluster.local:7100, yb-master-0.yb-masters.yb-demo-us-central1-b.svc.cluster.local:7100, yb-master-0.yb-masters.yb-demo-us-east1-b.svc.cluster.local:7100"

storage:
  master:
    storageClass: "standard-us-central1-b"
  tserver:
    storageClass: "standard-us-central1-b"

replicas:
  master: 1
  tserver: 1
  totalMasters: 3

gflags:
  master:
    placement_cloud: "gke"
    placement_region: "us-central1"
    placement_zone: "us-central1-b"
    leader_failure_max_missed_heartbeat_periods: 10
  tserver:
    placement_cloud: "gke"
    placement_region: "us-central1"
    placement_zone: "us-central1-b"
    leader_failure_max_missed_heartbeat_periods: 10
```

Copy the contents below to a file named `overrides-us-east1-b.yaml`.

```sh
isMultiAz: True

AZ: us-east1-b

masterAddresses: "yb-master-0.yb-masters.yb-demo-us-west1-b.svc.cluster.local:7100, yb-master-0.yb-masters.yb-demo-us-central1-b.svc.cluster.local:7100, yb-master-0.yb-masters.yb-demo-us-east1-b.svc.cluster.local:7100"

storage:
  master:
    storageClass: "standard-us-east1-b"
  tserver:
    storageClass: "standard-us-east1-b"

replicas:
  master: 1
  tserver: 1
  totalMasters: 3

gflags:
  master:
    placement_cloud: "gke"
    placement_region: "us-east1"
    placement_zone: "us-east1-b"
    leader_failure_max_missed_heartbeat_periods: 10
  tserver:
    placement_cloud: "gke"
    placement_region: "us-east1"
    placement_zone: "us-east1-b"
    leader_failure_max_missed_heartbeat_periods: 10
```

### Install YugabyteDB

Now create the overall YugabyteDB cluster in such a way that one third of the nodes are hosted in each Kubernetes cluster.

```sh
$ helm install yb-demo-us-west1-b yugabytedb/yugabyte \
 --namespace yb-demo-us-west1-b \
 -f overrides-us-west1-b.yaml \
 --kube-context gke_yugabyte_us-west1-b_yugabytedb1 --wait
```

```sh
$ helm install yb-demo-us-central1-b yugabytedb/yugabyte \
 --namespace yb-demo-us-central1-b \
 -f overrides-us-central1-b.yaml \
 --kube-context gke_yugabyte_us-central1-b_yugabytedb2 --wait
```

```sh
$ helm install yb-demo-us-east1-b yugabytedb/yugabyte \
 --namespace yb-demo-us-east1-b \
 -f overrides-us-east1-b.yaml \
 --kube-context gke_yugabyte_us-east1-b_yugabytedb3 --wait
```

## 4. Check the cluster status

You can check the status of the cluster using various commands noted below.

Check the pods.

```sh
$ kubectl get pods -n yb-demo-us-west1-b --context gke_yugabyte_us-west1-b_yugabytedb1
```

```sh
kubectl get pods -n yb-demo-us-central1-b --context gke_yugabyte_us-central1-b_yugabytedb2
```

```sh
kubectl get pods -n yb-demo-us-east1-b --context gke_yugabyte_us-east1-b_yugabytedb3
```

Check the services.

```sh
$ kubectl get services -n yb-demo-us-west1-b --context gke_yugabyte_us-west1-b_yugabytedb1
```

```
NAME                 TYPE           CLUSTER-IP      EXTERNAL-IP   PORT(S)                                        AGE
yb-master-ui         LoadBalancer   10.31.250.228   35.185.207.11   7000:31185/TCP                                 91m
yb-masters           ClusterIP      None            <none>          7100/TCP,7000/TCP                              91m
yb-tserver-service   LoadBalancer   10.31.247.185   34.83.192.162   6379:31858/TCP,9042:30444/TCP,5433:30854/TCP   91m
yb-tservers          ClusterIP      None            <none>          7100/TCP,9000/TCP,6379/TCP,9042/TCP,5433/TCP   91m
```

```sh
kubectl get services -n yb-demo-us-central1-b --context gke_yugabyte_us-central1-b_yugabytedb2
```

```sh
kubectl get services -n yb-demo-us-east1-b --context gke_yugabyte_us-east1-b_yugabytedb3
```

Access the yb-master Admin UI for the cluster at `http://<external-ip>:7000` where `external-ip` refers to one of the `yb-master-ui` services. Note that you can use any of the above 3 services for this purpose since all of them will show the same cluster metadata.

![mz-ybmaster](/images/deploy/kubernetes/gke-multicluster-ybmaster.png)  

## 5. Configure region-aware replica placement

Default replica placement policy treats every yb-tserver as equal irrespective of its `placement_*` flags. Go to `http://<external-ip\>:7000/cluster-config` to confirm that the default configuration is still in effect.

![before-regionaware](/images/deploy/kubernetes/gke-multicluster-before-regionaware.png)

Run the following command to make the replica placement region/cluster-aware so that one replica is placed on each region/cluster.

```sh
kubectl exec -it -n yb-demo-us-west1-b --context gke_yugabyte_us-west1-b_yugabytedb1 yb-master-0 -- bash \
-c "/home/yugabyte/master/bin/yb-admin --master_addresses yb-master-0.yb-masters.yb-demo-us-west1-b.svc.cluster.local:7100,yb-master-0.yb-masters.yb-demo-us-central1-b.svc.cluster.local:7100,yb-master-0.yb-masters.yb-demo-us-east1-b.svc.cluster.local:7100 modify_placement_info gke.us-west1.us-west1-b,gke.us-central1.us-central1-b,gke.us-east1.us-east1-b 3"
```

To see the new configuration, go to `http://<external-ip>:7000/cluster-config`.

![after-regionaware](/images/deploy/kubernetes/gke-multicluster-after-regionaware.png)

## 6. Connect using YugabyteDB shells

To connect and use the YSQL Shell (`ysqlsh`), run the following command.

```sh
$ kubectl exec -n yb-demo-us-west1-b --context gke_yugabyte_us-west1-b_yugabytedb1 \
 -it yb-tserver-0 -- ysqlsh -h yb-tserver-0.yb-tservers.yb-demo-us-west1-b
```

To connect and use the YCQL Shell (`ycqlsh`), run the following command.

```sh
$ kubectl exec -n yb-demo-us-west1-b --context gke_yugabyte_us-west1-b_yugabytedb1 \
-it yb-tserver-0 -- ycqlsh yb-tserver-0.yb-tservers.yb-demo-us-west1-b
```

You can follow the [Explore YSQL](../../../../../quick-start/explore-ysql) tutorial and then go to the `http://<external-ip>:7000/tablet-servers` page of the yb-master Admin UI to confirm that tablet peers and their leaders are placed evenly across all three zones for both user data and system data.

![mz-ybtserver](/images/deploy/kubernetes/gke-multicluster-ybtserver.png) 

## 7. Connect using external clients

To connect an external program, get the load balancer `EXTERNAL-IP` address of one of the `yb-tserver-service` service and connect to the 5433 / 9042 ports for YSQL / YCQL services respectively.

```sh
$ kubectl get services -n yb-demo-us-west1-b --context gke_yugabyte_us-west1-b_yugabytedb1
```

```
NAME                 TYPE           CLUSTER-IP      EXTERNAL-IP   PORT(S)                                        AGE
...
yb-tserver-service   LoadBalancer   10.31.247.185   34.83.192.162   6379:31858/TCP,9042:30444/TCP,5433:30854/TCP   91m
...
```

## 8. Test DB cluster resilience in face of region failures

It’s time to test the resilience of the DB cluster when subjected to the complete failure of one region. We will simulate such a failure by setting the replica count of the YugabyteDB StatefulSets to 0 for the `us-central1` region.

```sh
kubectl scale statefulset yb-tserver --replicas=0 -n yb-demo-us-central1-b \
 --context gke_yugabyte_us-central1-b_yugabytedb2

kubectl scale statefulset yb-master --replicas=0 -n yb-demo-us-central1-b \
 --context gke_yugabyte_us-central1-b_yugabytedb2
```

If you re-run the queries from Step 6 after re-connecting to the nodes in the `us-west1` region, you will see that there is absolutely no impact to the availability of the cluster and the data stored therein. However, there is higher latency for some of the transactions since the farthest `us-east1` region now has to be involved in the write path. In other words, the database cluster is fully protected against region failures but may temporarily experience higher latency. This is a much better place to be than a complete outage of the business-critical database service. The post [Understanding How YugabyteDB Runs on Kubernetes](https://blog.yugabyte.com/understanding-how-yugabyte-db-runs-on-kubernetes/) details how YugabyteDB self-heals the replicas when subjected to the failure of a fault domain (the cloud region in this case) by auto-electing a new leader for each of the impacted shards in the remaining fault domains. The cluster goes back to its original configuration as soon as the nodes in the lost region become available again.
