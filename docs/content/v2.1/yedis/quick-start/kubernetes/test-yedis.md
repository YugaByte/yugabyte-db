

* Initialize YEDIS API in the YugabyteDB universe you just set up by running the following `yb-admin` command.

```sh
$ kubectl exec -it yb-master-0 -- /home/yugabyte/bin/yb-admin --master_addresses yb-master-0.yb-masters.default.svc.cluster.local:7100 setup_redis_table
```

```
...
I0127 19:38:10.358551   115 client.cc:1292] Created table system_redis.redis of type REDIS_TABLE_TYPE
I0127 19:38:10.358872   115 yb-admin_client.cc:400] Table 'system_redis.redis' created.
```

Run `redis-cli` to connect to the service.

You can do this as shown below.

```sh
$ kubectl exec -it yb-tserver-0 -- /home/yugabyte/bin/redis-cli
```

```
127.0.0.1:6379>
```

Run a YEDIS command to verify it is working.

```sql
127.0.0.1:6379> PING
```

```
"PONG"
```
