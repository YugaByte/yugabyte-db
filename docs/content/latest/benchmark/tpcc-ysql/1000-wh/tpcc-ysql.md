### TPC-C Load Phase

Before starting the workload, you will need to load the data first. Make sure
to replace the IP addresses with that of the nodes in the cluster. Loader
threads allow us to configure the number of threads used to load the data. For
a 3 node c5d.4xlarge cluster, loader threads value of 48 was the most optimal.

```sh
$ ./tpccbenchmark --create=true --load=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=1000 \
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
      <td>~3.5 hours</td>
    </tr>
    <tr>
      <td>Data Set Size</td>
      <td>~420 GB</td>
    </tr>
  </tbody>
</table>

Tune the --loaderthreads parameter for higher parallelism during the load, based on the number and type of nodes in the cluster. The value specified here, 48 threads, is optimal for a 3-node cluster of type c5d.4xlarge (16 vCPUs). For larger clusters, or machines with more vCPUs, increase this value accordingly. For clusters with a replication factor of 3, a good approximation is to use the number of cores you have across all the nodes in the cluster.

### TPC-C Execute Phase

You can then run the workload against the database as follows:

```sh
$ ./tpccbenchmark --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=1000
```

You can also load and run the benchmark in a single step:
```sh
$ ./tpccbenchmark --create=true --load=true --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=1000 \
  --loaderthreads 48
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
      <td>12,563.07</td>
    </tr>
    <tr>
      <td>Efficiency</td>
      <td>97.69%</td>
    </tr>
    <tr>
      <td>Latencies</td>
      <td>
        New Order<br />
        Avg: 325.378 msecs, p99: 3758.859 msecs<br />
        Payment<br />
        Avg: 277.539 msecs, p99: 12667.048 msecs<br />
        OrderStatus<br />
        Avg: 174.173 msecs, p99: 4968.783 msecs<br />
        Delivery<br />
        Avg: 310.19 msecs, p99: 5259.951 msecs<br />
        StockLevel<br />
        Avg: 652.827 msecs, p99: 8455.325 msecs
      </td>
    </tr>
  </tbody>
</table>


Once the execution is done the TPM-C number along with the efficiency is printed.

```
17:18:58,728 (DBWorkload.java:955) INFO  - Throughput: Results(nanoSeconds=1800000716759, measuredRequests=842216) = 467.8975914612168 requests/sec reqs/sec
17:18:58,728 (DBWorkload.java:956) INFO  - Num New Order transactions : 376892, time seconds: 1800
17:18:58,728 (DBWorkload.java:957) INFO  - TPM-C: 12,563.07
17:18:58,728 (DBWorkload.java:958) INFO  - Efficiency : 97.69%
17:18:59,006 (DBWorkload.java:983) INFO  - NewOrder, Avg Latency: 325.378 msecs, p99 Latency: 3758.859 msecs
17:18:59,138 (DBWorkload.java:983) INFO  - Payment, Avg Latency: 277.539 msecs, p99 Latency: 12667.048 msecs
17:18:59,147 (DBWorkload.java:983) INFO  - OrderStatus, Avg Latency: 174.173 msecs, p99 Latency: 4968.783 msecs
17:18:59,166 (DBWorkload.java:983) INFO  - Delivery, Avg Latency: 310.19 msecs, p99 Latency: 5259.951 msecs
17:18:59,182 (DBWorkload.java:983) INFO  - StockLevel, Avg Latency: 652.827 msecs, p99 Latency: 8455.325 msecs
17:18:59,183 (DBWorkload.java:792) INFO  - Output Raw data into file: results/oltpbench.csv
```
