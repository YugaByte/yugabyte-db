### TPC-C Load Phase

Before starting the workload, you will need to load the data first. Make sure
to replace the IP addresses with that of the nodes in the cluster.

```sh
$ ./tpccbenchmark --create=true --load=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=1000 \
  --loaderthreads 48
```

### TPC-C Execute Phase

You can then run the workload against the database as follows:

```sh
$ ./tpccbenchmark --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=1000 \
  --memory=64G
```

Make sure to run the tool on a machine that has about `64GB` RAM.

You can also load and run the benchmark in a single step:
```sh
$ ./tpccbenchmark --create=true --load=true --execute=true \
  --nodes=127.0.0.1,127.0.0.2,127.0.0.3 \
  --warehouses=1000 \
  --loaderthreads 48 \
  --memory=64G
```

### TPC-C Benchmark Results

Once the execution is done the TPM-C number along with the efficiency is printed.

```
00:40:02,921 (DBWorkload.java:880) INFO  - Rate limited reqs/s: Results(nanoSeconds=1200000760106, measuredRequests=544170) = 453.47471275929166 requests/sec
00:40:02,921 (DBWorkload.java:885) INFO  - Num New Order transactions : 223376, time seconds: 1800
00:40:02,921 (DBWorkload.java:886) INFO  - TPM-C: 11205
00:40:02,921 (DBWorkload.java:887) INFO  - Efficiency : 87.1306376360809
01:25:34,671 (DBWorkload.java:722) INFO  - Output Raw data into file: results/oltpbench.csv
```
