---
title: Connect clients to Yugabyte Cloud clusters
headerTitle: Connect to Yugabyte Cloud clusters
linkTitle: Connect to clusters
description: Connect to clusters in Yugabyte Cloud using Yugabyte Cloud shells, remote clients, and third party clients.
beta: /latest/faq/general/#what-is-the-definition-of-the-beta-feature-tag
aliases:
  - /latest/deploy/yugabyte-cloud/connect-to-clusters/
menu:
  latest:
    identifier: connect-to-clusters
    parent: yugabyte-cloud
    weight: 645
isTocNested: true
showAsideToc: true
---

Connect to your YugabyteDB clusters in Yugabyte Cloud using Yugabyte Cloud shells, remote clients, and third party clients.

- [YSQL shell (`ysqlsh`)](#ysql-shell-ysqlsh)
  - [Use the YSQL cloud shell](#use-the-ysql-cloud-shell)
  - [Connect using a remote YSQL shell](#connect-using-a-remote-ysql-shell)
- [YCQL shell (`ycqlsh`)](#ycql-shell-ycqlsh)
  - [Use the YCQL cloud shell](#use-the-ycql-cloud-shell)
  - [Connect using a remote YCQL shell](#connect-using-a-remote-ycql-shell)

## YSQL shell (`ysqlsh`)

### Use the YSQL cloud shell

To access the YSQL cloud shell, follow these steps:

1. Go to your cluster by clicking **Clusters > Go to cluster**.
2. Click **Connect** (above the dashboard). The **Connection Info** window appears.
3. Select **YSQL Shell** and then click **Run in Cloud Shell**. The YSQL shell prompt appears in a separate page and is ready to use.

```
ysqlsh (11.2-YB-2.2.0.0-b0)
Type "help" for help.

yugabyte=#
```

Related information:

- [ysqlsh](../../admin/ysqlsh) — Overview of the command line interface (CLI), syntax, and commands.
- [YSQL API](../../api/ysql) — Reference for supported YSQL statement, data types, functions, and operators.

### Connect using a remote YSQL shell

Follow these steps to connect to your remote cluster using the [YSQL shell (`ysqlsh`)](../../admin/ysqlsh/):

1. Log into Yugabyte Cloud and click **Cluster** in the navigation bar. The list of available clusters appears.
2. Click **Go to cluster** for your cluster. The Yugabyte Cloud Console appears.
3. Click **Connect** to get the user credentials you need to access the remote cluster. The Connect dialog appears with user credentials
   use **COPY** to copy a generated command that you can use to access the YCQL shell. The generated command includes flags specifying
   the host (`-h`), port (`-p`), username (`-U`), and database (`-d`). To specify the password The generated command connects to the default database (`yugabyte`).

   Here's an example of the generated command:

    ```sh
    PGPASSWORD=sx73qlpc ./bin/ysqlsh -h 35.236.85.97 -p 12201 -U admin -d yugabyte
    ```

4. Change directories to the Yugabyte home directory.
5. Paste and run the generated command saved in step 2.

    ```sh
    $ PGPASSWORD=sx73qlpc ./bin/ysqlsh -h 35.236.85.97 -p 12201 -U admin -d yugabyte
    ```

    ```
    ysqlsh (11.2-YB-2.1.0.0-b0)
    Type "help" for help.

    yugabyte=#
    ```

The local [YSQL shell (`ysqlsh`)](../../admin/ysqlsh/) opens connected to the remote cluster.

## YCQL shell (`ycqlsh`)

### Use the YCQL cloud shell

To access the YCQL cloud shell, follow these steps:

1. Go to your cluster by clicking **Clusters > Go to cluster**.
2. Click **Connect** (above the dashboard). The **Connection Info** window appears.
3. Select **YCQL Shell** and then click **Run in Cloud Shell**. The YCQL shell prompt appears in a separate page and is ready to use.

```
Connected to local cluster at acd4d7803e26911ea85700affa0900f1-1535195171.us-west-2.elb.amazonaws.com:12600.
[ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
Use HELP for help.
admin@ycqlsh>
```

Related information:

- [ycqlsh](../../admin/ycqlsh) — Overview of the command line interface (CLI), syntax, and commands.
- [YCQL API](../../api/ycql) — Reference for supported YCQL statement, data types, and functions.

### Connect using a remote YCQL shell

Follow these steps to connect to your remote cluster using the [YCQL shell (`ycqlsh`)](../../admin/ycqlsh/):

1. Log into Yugabyte Cloud and click **Cluster** in the navigation bar. The list of available clusters appears.
2. Click **Go to cluster** for your cluster. The Yugabyte Cloud Console appears.
3. Click **Connect** to get the user credentials you need to access the remote cluster. The Connect dialog appears with user credentials
   use **COPY** to copy a generated command that you can use to access the YCQL shell. The generated command includes flags specifying
   the host (`-h`), port (`-p`), username (`-U`), and database (`-d`). To specify the password The generated command connects to the default database (`yugabyte`).

    Here's an example of the generated command:

    ```sh
    ./bin/ycqlsh 35.236.85.97 12200 -u admin -p sx73qlpc
    ```

4. Change directories to the Yugabyte home directory.
5. Paste and run the generated command you copied in step 3.

    ```sh
    $ ./bin/ycqlsh 35.236.85.97 12200 -u admin -p sx73qlpc
    ```

    ```
    Connected to local cluster at 35.236.85.97:12200.
    [ycqlsh 5.0.1 | Cassandra 3.9-SNAPSHOT | CQL spec 3.4.2 | Native protocol v4]
    Use HELP for help.
    admin@ycqlsh>
    ```

The local [YCQL shell (`ycqlsh`)](../../admin/ycqlsh/) opens connected to the remote cluster.

## Connect using third party clients

Because YugabyteDB is PostgreSQL-compatible, you can use third party PostgreSQL clients to connect to your YugabyteDB clusters in Yugabyte Cloud.
To create connections to your cluster in Yugabyte Cloud, follow the client's configuration steps for PostgreSQL, but use the values for host, port, username,
and password available in the Connect dialog for your cluster.

To see the host, port, username, and password required to connect:

1. Click **Clusters** an then click **Go to cluster** for the cluster you want to connect to.
2. Click **Connect**. The **Connect** dialog appears, displaying the host, port, and user credentials for the default user (`admin`).
3. To get the user credentials for a different user, click **Database Access** and then click **INFO** for that user.

For detailed steps for configuring popular third party tools, see [Third party tools](../../tools/). In that section, configuration steps
are included for the following tools:

- [DBeaver](../../tools/dbeaver)
- [DbSchema](../../tools/dbschema)
- [pgAdmin](../../tools/pgadmin)
- [SQL Workbench/J](../../tools/sql-workbench)
- [TablePlus](../../tools/tableplus)
- [Visual Studio Workbench](../../tools/visualstudioworkbench)
