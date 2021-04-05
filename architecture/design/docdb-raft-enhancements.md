# Raft Enhancements in DocDB

> **NOTE:** This design doc is still a work in progress.


Recall that the storage layer of YugabyteDB is a distributed document store called DocDB. The architecture of DocDB is inspired by Google Spanner. This design doc outlines the various Raft enhancements in DocDB.

YugabyteDB uses Raft consensus without any atomic clocks in order to achieve single-key linearizability while maintaining high read performance. **Linearizability** is one of the strongest single-key consistency models, and implies that every operation appears to take place atomically and in some total linear order that is consistent with the real-time ordering of those operations. In other words, the following should be true of operations on a single key:
* Operations can execute concurrently, but the state of the database at any point in time must appear to be the result of some totally ordered, sequential execution of operations.
* If operation A completes before operation B begins, then B should logically take effect after A.


| Feature/Enhancement Name                        | Purpose       |
| ----------------------------------------------- | ------------- |
| [Leader leases](#leader-leases)                 | Improves read performance by decreasing latency of read queries. |
| [Group commits](#group-commits)                 | Improves write performance by increasing write throughput. |
| [Leader balancing](#leader-balancing) | Improves read and write performance with optimal utilization of nodes in the cluster. |
| [Affinitized Leaders](#affinitized-leaders) | Improves read and write performance in geo-distributed scenarios by allowing placing leaders closer to the the location of the app. |
| Configurable missed heartbeats    | Enables multi-region and hybrid-cloud deployments where network latency between nodes is high. |
| Integrating Hybrid Logical Clocks | Enables cross-shard transactions as a building block for a software-defined atomic clock for a cluster. |
| MVCC Fencing             | Guarantee safety of writes in leader failure scenarios. |
| Non-Voting Observer Nodes         | Enable multi-region deployments that require low-latency writes and low-latency follower reads. |

## Leader Leases

In order to serve reads, the Raft consensus algorithm requires the current Raft-leader to successfully heartbeat to a majority of peers after it receives the query but before responding to it. This is done to ensure that it is still the Raft-leader, especially in the presence of network-partitions. One possible sequence of operations is shown below.

```
                                       ╔════════════╗     ╔════════════╗
                                       ║ 3a) Heart- ║     ║ 4b) Heart- ║
                                       ║     beat   ║     ║     beat   ║
                                       ║    request ║     ║   response ║
                                       ║    from B  ║     ║    from A  ║ 
Node A                                 ╚═══════════╦╝     ╚═══╦════════╝             Increasing Time
---------------------------------------------------^----------|------------------------------------->
                                                   |           \
╔══════════════╗  ╔═══════════╗ ╔════════════╗    /             \      ╔═════════════════════════╗
║ 1) Node wins ║  ║ 2) Read   ║ ║ 3) Heart-  ║   /               |     ║ 5) Got majority heart-  ║
║    leader    ║  ║    query  ║ ║   beat to  ║  /                |     ║    beat, can respond    ║
║    election  ║  ║   arrives ║ ║   majority ║ /                 |     ║    to client read query ║
╚═════╦════════╝  ╚═════╦═════╝ ╚═══════════╦╝/                  |     ╚═════╦═══════════════════╝  
------V-----------------V-------------------V|---------------^---V-----------V---------^------------>
Node B                                        \           ╔══╩════════════╗            |   
                                               \          ║ 4a) Heart-    ║           /
                                                \         ║     beat resp ║          /
                                                |         ║     from self ║         /
                                                |         ╚═══════════════╝        /  Increasing Time
------------------------------------------------V---------------------------------|----------------->
Node C                                 ╔════════╩═══╗                      ╔══════╩═════╗
                                       ║ 3b) Heart- ║                      ║ 6) Heart-  ║
                                       ║     beat   ║                      ║     beat   ║
                                       ║    request ║                      ║   response ║
                                       ║    from B  ║                      ║     from C ║
                                       ╚════════════╝                      ╚════════════╝

```

### What causes the high latency?

In the figure above, the cause of the high latency is the step 4b, which introduces a network hop in the read path by forcing the Raft leader to wait for the heartbeat response from  one of the Raft followers.

> **NOTE**: If the nodes A, B and C are in different regions, the network latency between them is often very large. In this scenario, the read latencies would be very high.


### How do leader leases work?

Leader leases eliminate the need for the extra heartbeat from the remote peers (step 4b in the diagram above), without sacrificing safety.

In YugabyteDB, a newly elected leader cannot serve reads (or initiate writing a no-op Raft operation which is a prerequisite to accepting writes) until it has acquired a leader lease. During a leader election, a voter must propagate the longest remaining duration time of an old leader’s lease known to that voter to the new candidate it is voting for. Upon receiving a majority of votes, the new leader must wait out the old leader’s lease duration before considers itself as having the lease. The old leader, upon the expiry of its leader lease, steps down and no longer functions as a leader. The new leader continuously extends its leader lease as a part of Raft replication. Typically, leader leases have a short duration, for example the default in YugabyteDB is 2 seconds.

The sequence of steps is as follows:

* A leader computes a leader lease time interval
* The timer for this time interval starts counting down on the existing leader
* The leader sends the lease time interval to the followers using an RPC as a part of Raft replication. The RPC message itself incurs a time delay.
* Only after the RPC delay do the followers receive the lease time interval and start their countdown.
* Each node performs a countdown based on its monotonic clock.
* It is easy to adjust the time delta by the max rate of clock drift on each node to take care of the variation between monotonic clock rates on different nodes.

This is shown diagrammatically in the diagram below.
![Raft leader leases timing sequence](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/architecture/design/images/docdb-raft-leader-leases-timing-sequence.png)

> **NOTE**: The above sequence creates a time window where the old leader steps down and the new leader does not step up, causing an unavailability window. In practice, however, this may not be hugely impactful since the unavailability window occurs only during failure scenarios (which are comparatively rare events) and the time window itself is quite “small” as observed by the end user. The unavailability window is bounded by the following equation:

  ```
  max-unavailability = max-drift-between-nodes * leader-lease-interval + rpc-message-delay
  ```

## Group Commits

Per the Raft algorithm:

> Each new entry being appended into the Raft log is assigned a monotonically increasing operation id, which is a tuple consisting of (term, index).

Let us assume the scenario where term does not change. This implies that in a naive implementation, each client issued update operation is treated as a separate Raft record entry, leading to a lot of RPCs between the Raft members. In turn, this would result in reduced write throughput in scenarios with highly concurrent updates because each write operation to a tablet (Raft group) would have to wait for all the previous write operations to complete. Additionally, cases when each update is small (in terms of the total payload size) would result in the network might not be being optimally utilized since there would be a lot of small packets.

To utilize the network better, DocDB batches multiple outstanding updates into a single record. This batching of updates in order to commit them to the Raft log is referred to as a **group commit**. The idea here is to group all the incoming update requests from the client into a new Raft message batch and send them over the network together. While one Raft replica batch is being replicated to a majority, all new incoming updates are grouped into a new Raft message batch. This is shown in the diagram below. 

![Group commits in Raft](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/architecture/design/images/docdb-raft-group-commit.png)

The entries inside a single Raft message batch are required to be ordered as well. This is done by introducing another entry into the Raft id tuple called the *op id*, which preserves the order of the client updates.

## Leader Balancing

YugabyteDB uses the Raft algorithm to implement a consistent and fault-tolerant write-ahead log. Per the Raft consensus algorithm, the various nodes that host the different Raft replicas (called tablet peers) first elect a leader. This results in one of the nodes becoming the leader of the tablet, and the others become followers. There could now arise scenarios where the distribution of Raft leaders and followers per node is uneven.

YugabyteDB tries to balance the Raft leaders and followers evenly across the nodes of the cluster. This is done as a background operation in a throttled manner so as to not impact foreground latencies or throughput.

> **NOTE**: This is the default behavior. There are user defined policies that change the distribution of leaders, see the *affinitized leaders* section for more details.



[![Analytics](https://yugabyte.appspot.com/UA-104956980-4/architecture/design/docdb-raft-enhancements.md?pixel&useReferer)](https://github.com/yugabyte/ga-beacon)
