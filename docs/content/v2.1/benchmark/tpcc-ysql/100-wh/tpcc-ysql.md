### TPC-C Load Phase

Before starting the workload, you will need to load the data first. Make sure
to replace the IP addresses with that of the nodes in the cluster.

```sh
$ ./tpccbenchmark --create=true --load=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3
  --warehouses=100 \
  --loaderthreads 48
```

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

### TPC-C Benchmark Results

Once the execution is done the TPM-C number along with the efficiency is printed.

```
01:25:34,669 (DBWorkload.java:880) INFO  - Rate limited reqs/s: Results(nanoSeconds=1800000482491, measuredRequests=423634) = 235.35215913594521 requests/sec
01:25:34,669 (DBWorkload.java:885) INFO  - Num New Order transactions : 186826, time seconds: 1800
01:25:34,669 (DBWorkload.java:886) INFO  - TPM-C: 6227
01:25:34,669 (DBWorkload.java:887) INFO  - Efficiency : 96.84292379471229
01:25:34,671 (DBWorkload.java:722) INFO  - Output Raw data into file: results/oltpbench.csv
```
