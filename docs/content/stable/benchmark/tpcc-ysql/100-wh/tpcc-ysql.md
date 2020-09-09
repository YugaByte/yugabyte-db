### TPC-C Load Phase

Before starting the workload, you will need to load the data first. Make sure
to replace the IP addresses with that of the nodes in the cluster. Loader
threads allow us to configure the number of threads used to load the data. For
a 3 node c5d.4xlarge cluster, loader threads value of 48 was the most optimal.

```sh
$ ./tpccbenchmark --create=true --load=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=100 \
  --loaderthreads 48
```

<table>
  <tbody>
    <tr>
      <td>Cluster</td>
      <td>3 nodes of type `c5d.4xlarge`</td>
    </tr>
    <tr>
      <td>Loader threads</td>
      <td>48</td>
    </tr>
    <tr>
      <td>Loading Time</td>
      <td>~20 minutes</td>
    </tr>
    <tr>
      <td>Data Set Size</td>
      <td>~80 GB</td>
    </tr>
  </tbody>
</table>

Tune the --loaderthreads parameter for higher parallelism during the load, based on the number and type of nodes in the cluster. The value specified here, 48 threads, is optimal for a 3-node cluster of type c5d.4xlarge (16 vCPUs). For larger clusters, or machines with more vCPUs, increase this value accordingly.

### TPC-C Execute Phase

You can then run the workload against the database as follows:

```sh
$ ./tpccbenchmark --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=100
```

You can also load and run the benchmark in a single step:
```sh
$ ./tpccbenchmark --create=true --load=true --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --loaderthreads 48 \
  --warehouses=100
```

## 4. TPC-C Benchmark Results

<table>
  <tbody>
    <tr>
      <td>Cluster</td>
      <td>3 nodes of type `c5d.4xlarge`</td>
    </tr>
    <tr>
      <td>TPMC</td>
      <td>1271.77</td>
    </tr>
    <tr>
      <td>Efficiency</td>
      <td>98.89%</td>
    </tr>
    <tr>
      <td>Latencies</td>
      <td>
        New Order<br />
        Avg: 68.265 msecs, p99: 574.339 msecs<br />
        Payment<br />
        Avg: 19.969 msecs, p99: 475.311 msecs<br />
        OrderStatus<br />
        Avg: 13.821 msecs, p99: 571.414 msecs<br />
        Delivery<br />
        Avg: 67.384 msecs, p99: 724.67 msecs<br />
        StockLevel<br />
        Avg: 114.032 msecs, p99: 263.849 msecs
      </td>
    </tr>
  </tbody>
</table>

Once the execution is done the TPM-C number along with the efficiency is printed.

```
04:54:54,560 (DBWorkload.java:955) INFO  - Throughput: Results(nanoSeconds=1800000866600, measuredRequests=85196) = 47.33108832382159 requests/sec reqs/sec
04:54:54,560 (DBWorkload.java:956) INFO  - Num New Order transactions : 38153, time seconds: 1800
04:54:54,560 (DBWorkload.java:957) INFO  - TPM-C: 1,271.77
04:54:54,560 (DBWorkload.java:958) INFO  - Efficiency : 98.89%
04:54:54,596 (DBWorkload.java:983) INFO  - NewOrder, Avg Latency: 68.265 msecs, p99 Latency: 574.339 msecs
04:54:54,615 (DBWorkload.java:983) INFO  - Payment, Avg Latency: 19.969 msecs, p99 Latency: 475.311 msecs
04:54:54,616 (DBWorkload.java:983) INFO  - OrderStatus, Avg Latency: 13.821 msecs, p99 Latency: 571.414 msecs
04:54:54,617 (DBWorkload.java:983) INFO  - Delivery, Avg Latency: 67.384 msecs, p99 Latency: 724.67 msecs
04:54:54,618 (DBWorkload.java:983) INFO  - StockLevel, Avg Latency: 114.032 msecs, p99 Latency: 263.849 msecs
04:54:54,619 (DBWorkload.java:792) INFO  - Output Raw data into file: results/oltpbench.csv
```
