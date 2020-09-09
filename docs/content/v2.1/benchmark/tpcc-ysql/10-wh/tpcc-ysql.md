### TPC-C Load Phase

Before starting the workload, you will need to load the data first. Make sure
to replace the IP addresses with that of the nodes in the cluster.

```sh
$ ./tpccbenchmark --create=true --load=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3
```

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

### TPC-C Benchmark Results

Once the execution is done the TPM-C number along with the efficiency is printed.

```
20:57:54,318 (DBWorkload.java:896) INFO  - Rate limited reqs/s: Results(nanoSeconds=1800000300980, measuredRequests=8528) = 4.737776985568823 requests/sec
20:57:54,318 (DBWorkload.java:901) INFO  - Num New Order transactions : 3799, time seconds: 1800
20:57:54,318 (DBWorkload.java:902) INFO  - TPM-C: 126
20:57:54,318 (DBWorkload.java:903) INFO  - Efficiency : 97.97822706065319
20:57:54,321 (DBWorkload.java:738) INFO  - Output Raw data into file: results/oltpbench.csv
```
