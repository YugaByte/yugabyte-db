---
title: Connect clients to YugabyteDB clusters
headerTitle: Connect to clusters
linkTitle: Connect to clusters
description: Connect to clusters in Yugabyte Cloud using locally installed YugabyteDB clients.
beta: /stable/faq/general/#what-is-the-definition-of-the-beta-feature-tag
aliases:
  - /stable/deploy/yugabyte-cloud/connect-to-clusters/
block_indexing: true
menu:
  stable:
    identifier: connect-to-clusters
    parent: yugabyte-cloud
    weight: 645
isTocNested: true
showAsideToc: true
---

You can connect to YugabyteDB clusters in Yugabyte Cloud with YugabyteDB clients and third party clients.

You can use the following YugabyteDB clients (locally installed) to connect to your remote clusters in Yugabyte Cloud:

- [YSQL shell (`ysqlsh`)](../../admin/ysqlsh/)
- [YCQL shell (`ycqlsh`)](../../admin/ycqlsh/)

{{< note title="Note" >}}

To use the YugabyteDB CLIs to connect to your remote Yugabyte Cloud clusters, you must have a local installation of
YugabyteDB. If you do not have a local installation, see [Install YugabyteDB](../../quick-start/install/).

{{< /note >}}

## Connect using the YSQL shell (ysqlsh)

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

## Connect using the YCQL shell (ycqlsh)

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
