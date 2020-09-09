### TPC-C Load Phase

Before starting the workload, you will need to load the data first. Make sure
to replace the IP addresses with that of the nodes in the cluster.

```sh
$ ./tpccbenchmark --create=true --load=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3
```

<table>
  <tbody>
    <tr>
      <td>Cluster</td>
      <td>3 nodes of type `c5d.large`</td>
    </tr>
    <tr>
      <td>Loader threads</td>
      <td>10</td>
    </tr>
    <tr>
      <td>Loading Time</td>
      <td>~13 minutes</td>
    </tr>
    <tr>
      <td>Data Set Size</td>
      <td>~20 GB</td>
    </tr>
  </tbody>
</table>

---
**NOTE**

The loading time for 10 warehouses on a cluster with 3 nodes of type c5d.4xlarge is around 3 minutes.

---

### TPC-C Execute Phase

You can then run the workload against the database as follows:

```sh
$ ./tpccbenchmark --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3
```

You can also load and run the benchmark in a single step:
```sh
$ ./tpccbenchmark --create=true --load=true --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3
```

## 4. TPC-C Benchmark Results

<table>
  <tbody>
    <tr>
      <td>Cluster</td>
      <td>3 nodes of type `c5d.large`</td>
    </tr>
    <tr>
      <td>TPMC</td>
      <td>127</td>
    </tr>
    <tr>
      <td>Efficiency</td>
      <td>98.75%</td>
    </tr>
    <tr>
      <td>Latencies</td>
      <td>
        New Order<br />
        Avg: 66.286 msecs, p99: 212.47 msecs<br />
        Payment<br />
        Avg: 17.406 msecs, p99: 186.884 msecs<br />
        OrderStatus<br />
        Avg: 7.308 msecs, p99: 86.974 msecs<br />
        Delivery<br />
        Avg: 66.986 msecs, p99: 185.919 msecs<br />
        StockLevel<br />
        Avg: 98.32 msecs, p99: 192.054 msecs
      </td>
    </tr>
  </tbody>
</table>

Once the execution is done the TPM-C number along with the efficiency is printed.

```
21:09:23,588 (DBWorkload.java:955) INFO  - Throughput: Results(nanoSeconds=1800000263504, measuredRequests=8554) = 4.752221526539232 requests/sec reqs/sec
21:09:23,588 (DBWorkload.java:956) INFO  - Num New Order transactions : 3822, time seconds: 1800
21:09:23,588 (DBWorkload.java:957) INFO  - TPM-C: 127
21:09:23,588 (DBWorkload.java:958) INFO  - Efficiency : 98.75%
21:09:23,593 (DBWorkload.java:983) INFO  - NewOrder, Avg Latency: 66.286 msecs, p99 Latency: 212.47 msecs
21:09:23,596 (DBWorkload.java:983) INFO  - Payment, Avg Latency: 17.406 msecs, p99 Latency: 186.884 msecs
21:09:23,596 (DBWorkload.java:983) INFO  - OrderStatus, Avg Latency: 7.308 msecs, p99 Latency: 86.974 msecs
21:09:23,596 (DBWorkload.java:983) INFO  - Delivery, Avg Latency: 66.986 msecs, p99 Latency: 185.919 msecs
21:09:23,596 (DBWorkload.java:983) INFO  - StockLevel, Avg Latency: 98.32 msecs, p99 Latency: 192.054 msecs
21:09:23,597 (DBWorkload.java:792) INFO  - Output Raw data into file: results/oltpbench.csv

```
