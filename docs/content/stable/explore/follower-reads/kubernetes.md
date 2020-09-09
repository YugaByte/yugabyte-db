## 1. Create universe

If you have a previously running local universe, destroy it using the following.

```sh
$ ./yb-docker-ctl destroy
```

Start a new local universe with replication factor (RF) of `5`. This will create five nodes.

```sh
$ ./yb-docker-ctl create --rf 5
```

Add two more nodes.

```sh
$ ./yb-docker-ctl add_node
```

```sh
$ ./yb-docker-ctl add_node
```

## 2. Write some data

By default, the [YugabyteDB workload generator](https://github.com/yugabyte/yb-sample-apps) runs with strong read consistency where all data is read from the tablet leader. We are going to populate exactly one key with a 10 KB value into the system. Since the replication factor is 5, this key will get replicated to five of the seven nodes in the universe.

Run the `CassandraKeyValue` workload application to constantly update this key-value, as well as perform reads with strong consistency against the local universe.

```sh
$ docker cp yb-master-n1:/home/yugabyte/java/yb-sample-apps.jar .
```

```sh
$ java -jar ./yb-sample-apps.jar --workload CassandraKeyValue \
                                    --nodes localhost:9042 \
                                    --nouuid \
                                    --num_unique_keys 1 \
                                    --num_threads_write 1 \
                                    --num_threads_read 1 \
                                    --value_size 10240
```

In the above command, we have set the value of `num_unique_keys` to `1`, which means we are overwriting a single key `key:0`. We can verify this using `ycqlsh`:

```sh
$ docker exec -it yb-tserver-n1 /home/yugabyte/bin/ycqlsh
```

```
Connected to local cluster at localhost:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
```

```sql
ycqlsh> SELECT k FROM ybdemo_keyspace.cassandrakeyvalue;
```

```
 k
-------
 key:0

(1 rows)
```

## 3. Strongly consistent reads from tablet leaders

When performing strongly consistent reads as a part of the above command, all reads will be served by the tablet leader of the tablet that contains the key `key:0`. If you browse to the <a href='http://localhost:7000/tablet-servers' target="_blank">tablet-servers</a> page, you will see that all the requests are indeed being served by one YB-TServer:

![Reads from the tablet leader](/images/ce/tunable-reads-leader-docker.png)

## 4. Timeline consistent reads from tablet replicas

Stop the running `CassandraKeyValue` workload application, and then run the following variant of that  application. This command will do updates to the same key `key:0` which will go through the tablet leader, but it will reads from the replicas.

```sh
$ java -jar ./yb-sample-apps.jar --workload CassandraKeyValue \
                                    --nodes localhost:9042 \
                                    --nouuid \
                                    --num_unique_keys 1 \
                                    --num_threads_write 1 \
                                    --num_threads_read 1 \
                                    --value_size 10240 \
                                    --local_reads
```

This can be easily seen by refreshing the <a href='http://localhost:7000/tablet-servers' target="_blank">tablet-servers</a> page, where you will see that the writes are served by a single YB-TServer that is the leader of the tablet for the key `key:0` while multiple Yb-TServers which are replicas serve the reads.

![Reads from the tablet leader](/images/ce/tunable-reads-followers-docker.png)

## 5. Clean up (optional)

Optionally, you can shut down the local cluster created in Step 1.

```sh
$ ./yb-docker-ctl destroy
```
