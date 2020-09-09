
* Initialize the YEDIS API.

Setup the `redis_keyspace` keyspace and the `.redis` table so that this cluster becomes ready for Redis clients. Detailed output for the `setup_redis` command is available in the [yb-ctl Reference](../../admin/yb-ctl/#setup-redis).

```sh
$ ./bin/yb-ctl setup_redis
```

* Run redis-cli to connect to the service.

```sh
$ ./bin/redis-cli
```

```
127.0.0.1:6379> 
```

* Run a Redis command to verify it is working.

```sql
127.0.0.1:6379> PING
```

```
"PONG"
```
