---
title: Single-row transactions
headerTitle: Single-row ACID transactions
linkTitle: Single-row transactions
description: Learn how YugabyteDB offers ACID semantics for mutations involving a single row or rows that are located within a single shard.
aliases:
  - /architecture/transactions/single-row-transactions/
menu:
  latest:
    identifier: architecture-single-row-transactions
    parent: architecture-acid-transactions
    weight: 1154
isTocNested: false
showAsideToc: true
---

YugabyteDB offers ACID semantics for mutations involving a single row or rows that fall
within the same shard (partition, tablet). These mutations incur only one network roundtrip between the distributed consensus peers.

Even read-modify-write operations within a single row or single shard, such as the following incur
only one round trip in YugabyteDB.

```sql
   UPDATE table SET x = x + 1 WHERE ...
   INSERT ... IF NOT EXISTS
   UPDATE ... IF EXISTS
```

Note that this is unlike Apache Cassandra, which uses a concept called lightweight transactions to achieve correctness for these read-modify-write operations and incurs [4-network round trip latency](https://docs.datastax.com/en/cassandra/3.0/cassandra/dml/dmlLtwtTransactions.html).

## Reading the latest data from a recently elected leader

In a steady state, when the leader is appending and replicating log entries, the latest majority-replicated entry is exactly the committed one.  However, it is a bit more complicated right after a leader change.  When a new leader is elected in a tablet, it appends a no-op entry to the tablet's Raft log and replicates it, as described in the Raft protocol. Before this no-op entry is replicated, we consider the tablet unavailable for reading up-to-date values and accepting
read-modify-write operations.  This is because the new tablet leader needs to be able to guarantee
that all previous Raft-committed entries are applied to RocksDB and other persistent and in-memory
data structures, and it is only possible after we know that all entries in the new leader's log are
committed.

## Leader leases: reading the latest data in case of a network partition

Leader leases are a mechanism for a tablet leader to establish its authority for a certain short
time period in order to avoid the following inconsistency:

* The leader is network-partitioned away from its followers
* A new leader is elected
* The client writes a new value and the new leader replicates it
* The client reads a stale value from the old leader.

![A diagram showing a potential inconsistency in case of a network partition if leader leases are not present](/images/architecture/txn/leader_leases_network_partition.svg)

The leader lease mechanism in YugabyteDB prevents this inconsistency. It works as follows:

* With every leader-to-follower message (AppendEntries in Raft's terminology), whether replicating
  new entries or even an empty heartbeat message, the leader sends a "leader lease" request as a
  **time interval**, e.g.  could be "I want a 2-second lease". The lease duration is usually a
  system-wide parameter. For each peer, the leader also keeps track of the lease expiration time
  corresponding to each pending request (i.e. time when the request was sent + lease duration),
  which is stored in terms of **local monotonic time**
  ([CLOCK_MONOTONIC](https://linux.die.net/man/3/clock_gettime) in Linux).  The leader considers
  itself as a special case of a "peer" for this purpose.  Then, as it receives responses from
  followers, it maintains the majority-replicated watermark of these expiration times **as stored at request sending time**. The leader adopts this majority-replicated watermark as its lease
  expiration time, and uses it when deciding whether it can serve consistent read requests or accept
  writes.

* When a follower receives the above Raft RPC, it reads the value of its current **monotonic
  clock**, adds the provided lease interval to that, and remembers this lease expiration time, also
  in terms of its local monotonic time.  If this follower becomes the new leader, it is not allowed
  to serve consistent reads or accept writes until any potential old leader's lease expires.

* To guarantee that any new leader is aware of any old leader's lease expiration, another bit of
  logic is necessary. Each Raft group member records the latest expiration time of an old leader
  that it knows about (in terms of this server's local monotonic time). Whenever a server responds
  to a RequestVote RPC, it includes the largest **remaining amount of time** of any known old
  leader's lease in its response. This is handled similarly to the lease duration in a leader's
  AppendEntries request on the receiving server: at least this amount of time has to pass since the
  receipt of this request before the recipient can service up-to-date requests in case it becomes a
  leader.  This part of the algorithm is needed so that we can prove that a new leader will always
  know about any old leader's majority-replicated lease. This is analogous to Raft's correctness
  proof: there is always a server ("the voter") that received a lease request from the old leader
  and voted for the new leader, because the two majorities must overlap.

Note that we are not relying on any kind of clock synchronization for this leader lease
implementation, as we're only sending time intervals over the network, and each server operates in
terms of its local monotonic clock. The only two requirements to the clock implementation are:

* Bounded monotonic clock drift rate between different servers. E.g. if we use the standard Linux
  assumption of less than 500&micro;s per second drift rate, we could account for it by multiplying
  all delays mentioned above by 1.001.

* The monotonic clock does not freeze. E.g. if we're running on a VM which freezes temporarily, the
  hypervisor needs to refresh the VM's clock from the hardware clock when it starts running again.

The leader lease mechanism guarantees that at any point in time there is at most one server in any
tablet's Raft group that considers itself to be an up-to-date leader that is allowed to service
consistent reads or accept write requests.

## Safe timestamp assignment for a read request

Every read request is assigned a particular MVCC timestamp / hybrid time (let's call it
**ht_read**), which allows write operations to the same set of keys to happen in parallel with
reads. It is crucial, however, that the view of of the database *as of this timestamp* is not
updated by concurrently happening writes. In other words, once we've picked **ht_read** for a read
request, no further writes to the same set of keys can be assigned timestamps lower than or equal to
**ht_read**. As we mentioned above, we assign strictly increasing hybrid times to Raft log entries
of any given tablet.  Therefore, one way to assign **ht_read** safely would be to use the hybrid
time of the last committed record. As committed Raft log records are never overwritten by future
leaders, and each new leader reads the last log entry and updates its hybrid time, all future
records will have strictly higher hybrid times.

However, with this conservative timestamp assignment approach, **ht_read** can stay the same if
there is no write workload on this particular tablet. This will result in a client-observed anomaly
if [TTL(time-to-live)](../../../api/ycql/dml_insert/#insert-a-row-with-expiration-time-using-the-using-ttl-clause)
is being used: no expired values will disappear, as far as the client is concerned, until a new
record is written to the tablet. Then, a lot of old expired values could suddenly disappear. To
prevent this anomaly, we need to assign the read timestamp to be close to the current hybrid time
(which is in its turn close to the physical time) to preserve natural TTL semantics. We should
therefore try to choose **ht_read** to be the *highest possible timestamp* for which we can
guarantee that all future write operations in the tablet will have a strictly higher hybrid time
than that, even across leader changes.

For this, we need to introduce a concept of "hybrid time leader leases", similar to absolute-time
leader leases discussed in the previous section. With every Raft AppendEntries request to a
follower, whether it is a regular request or an empty / heartbeat request, a tablet leader computes
a "hybrid time lease expiration time", or **ht_lease_exp** for short, and sends that to the
follower. **ht_lease_exp** is usually computed as current hybrid time plus a fixed configured
duration (e.g. 2 seconds). By replying, followers acknowledge the old leader's exclusive authority
over assigning any hybrid times up to and including **ht_lease_exp**. Similarly to regular leases,
these hybrid time leases are propagated on votes. The leader maintains a majority-replicated
watermark, and considers itself to have replicated a particular value of a hybrid time leader lease
expiration if it sent that or a higher **ht_lease_exp** value to a majority of Raft group members.
For this purpose, the leader is always considered to have replicated an infinite leader lease to
itself.

### Definition of safe time

Now, suppose the current majority-replicated hybrid time leader lease expiration is
**replicated_ht_lease_exp**. Then the safe timestamp for a read request can be computed as the
maximum of:

* Last committed Raft entry's hybrid time
* One of:
  * If there are uncommitted entries in the Raft log: the minimum ofthe first uncommitted entry's
    hybrid time - &epsilon; (where &epsilon; is the smallest possibledifference in hybrid time)
    and **replicated_ht_lease_exp**.
  * If there are no uncommitted entries in the Raft log: the minimum of the current hybrid time and **replicated_ht_lease_exp**.

In other words, the last committed entry's hybrid time is always safe to read at, but for higher
hybrid times, the majority-replicated hybrid time leader lease is an upper bound. That is because we can only guarantee that no future leader will commit an entry with hybrid time less than **ht** if **ht < replicated_ht_lease_exp**.

Note that when reading from a single tablet, we never have to wait for the chosen **ht_read** to
become safe to read at because it is chosen as such already. However, if we decide to read a
consistent view of data across multiple tablets, **ht_read** could be chosen on one of them, and
we'll have to wait for that timestamp to become safe to read at on the second tablet. This will
typically happen very quickly, as the hybrid time on the second tablet's leader will be instantly
updated with the propagated hybrid time from the first tablet's leader, and in the common case we
will just have to wait for pending Raft log entries with hybrid times less than **ht_read** to be
committed.

## Propagating safe time from leader to followers for follower-side reads

YugabyteDB supports reads from followers to satisfy use cases that require an extremely low read
latency that can only be achieved by serving read requests in the data center closest to the client. This feature comes at the expense of potentially slightly stale results, and this is a trade-off that application developers have to make. Similarly to strongly-consistent leader-side reads, follower-side read operations also have to pick a safe read timestamp.

As before, "safe time to read at" means that no future writes are supposed to change the view of the data as of the read timestamp.  However, only the leader is able to compute the safe read time using the
algorithm described in the previous section.  Therefore, we propagate the latest safe time from
leaders to followers on AppendEntries RPCs. This means, for example, that follower-side reads
handled by a partitioned-away follower will see a "frozen" snapshot of the data, including values
with TTL specified not timing out. When the partition is healed, the follower will start getting
updates from the leader and will be able to return read results that would be very close to
up-to-date.
