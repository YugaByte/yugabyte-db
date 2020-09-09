
* Run `ycqlsh` to connect to the service.

You can do this as shown below.

```sh
$ docker exec -it yb-tserver-n1 /home/yugabyte/bin/ycqlsh yb-tserver-n1
```

```
Connected to local cluster at 127.0.0.1:9042.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
ycqlsh>
```

* Run a YCQL command to verify it is working.

```sql
ycqlsh> describe keyspaces;
```

```
system_schema  system_auth  system

ycqlsh>
```
