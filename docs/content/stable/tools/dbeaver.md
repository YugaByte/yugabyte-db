---
title: Use DBeaver with YugabyteDB YSQL
headerTitle: DBeaver
linkTitle: DBeaver
description: Use the DBeaver multi-platform database tool to explore and query YugabyteDB.
block_indexing: true
menu:
  stable:
    identifier: dbeaver
    parent: tools
    weight: 2710
isTocNested: true
showAsideToc: true
---

[DBeaver](https://dbeaver.io/) is a free (and [open source](https://github.com/dbeaver/dbeaver)) multi-platform, cross-platform database tool for developers, SQL programmers, database administrators, and analysts. DBeaver is written in Java, based on the [Eclipse](https://www.eclipse.org/) platform, and supports supports any database that has a JDBC driver, including PostgreSQL, MariaDB, and MySQL. And, using the PostgreSQL JDBC driver, you can use DBeaver with YugabyteDB.

The [DBeaver Community Edition](https://dbeaver.io/) includes these features:

- [Open source](https://github.com/dbeaver/dbeaver)
- Connection and metadata browser
- SQL query editor and executor
- Rich in-line data editor
- Entity relationship (ER) diagrams

The [DBeaver Enterprise Edition](https://dbeaver.com/) adds support for non-JDBC data sources, including MongoDB, Cassandra, and Redis.

![DBeaver application](/images/develop/tools/dbeaver/dbeaver-screenshot.png)

## Before you begin

Before getting started with DBeaver, make sure you meet the following prerequisites.

### YugabyteDB

Your YugabyteDB cluster should be up and running. If you're new to YugabyteDB, create a local cluster in less than five minutes following the steps in [Quick Start](../../../quick-start/install).

### Java Runtime Environment (JRE)

DBeaver requires a Java runtime (or JDK) for Java 8 or later. Some of the installers include a JRE, accessible only to DBeaver.

JDK and JRE installers for Linux, macOS, and Windows can be downloaded from [OpenJDK](http://jdk.java.net/), [AdoptOpenJDK](https://adoptopenjdk.net/), or [Azul Systems](https://www.azul.com/downloads/zulu-community/).

### PostgreSQL JDBC driver

The [PostgreSQL JDBC driver](https://jdbc.postgresql.org/) can be used with DBeaver to work with YugabyteDB databases. 

To connect DBeaver to a YugabyteDB cluster, you need the PostgreSQL JDBC driver installed. To download the current version that supports Java 8 or later, go to the [PostgreSQL JDBC Driver download](https://jdbc.postgresql.org/download.html) page.

## Install DBeaver

1. Download the distribution package for the operating system on your client computer from the [DBeaver downloads page](https://dbeaver.io/download/).
2. Install DBeaver following the steps on the [DBeaver Installation page](https://github.com/dbeaver/dbeaver/wiki/Installation).

## Configure DBeaver

### Configure the JDBC driver

1. Start the DBeaver application. The DBeaver application window appears.
2. On the menu, select **Database > Driver Manager**. The **Driver Manager** window appears.

![Driver Manager](/images/develop/tools/dbeaver/dbeaver-driver-manager.png)

3. Select **PostgreSQL** and then click **Copy**. The **Create new driver** window appears with a copy of the default PostgreSQL driver settings.

![Create new driver](/images/develop/tools/dbeaver/dbeaver-create-new-driver.png)

4. Make the following changes in the **Settings**:

    - **DriverName**: `YugabyteDB` — Default name is "PostgreSQL", but using "YugabyteDB" might help you not confuse this driver's settings with PostgreSQL connections using the PostgreSQL port of `5432`.
    - **Driver Type**: `PostgreSQL` (selected)
    - **Class Name**: `org.postgresql.Driver`
    - **URL Template**: `jdbc.postgresql://{host}[:{port}/[{database}]` (read-only)
    - **Default Port**: `5433` (Default is `5432`)

5. In the **Libraries** tab, select the PostgreSQL JDBC driver JAR file to be used.

6. Click **OK**. The **Create new driver** window closes. 

7. Verify that the new "YugabyteDB" driver appears in the **Driver Manager** listing and then click **Close**.

## Create a database connection

1. On the DBeaver menu, select **Database > New Connection**. The **Connect to database** window appears.

2. In the **Select your database** listing, select **YugabyteDB** and then click **Next**.

3. In the **Connection Settings**, add the following settings:

    - **Host**: `localhost`
    - **Port**: `5433`
    - **Database**: Clear the default value (`postgres`)
    - **User**: `yugabyte` (default is `postgres`)
    - **Password**: Leave blank if YSQL authentication is not enabled. If enabled, add the password for `yugabyte` (default is `yugabyte`).
    - **Show all databases**: Select this option.

4. Click **Test Connection** to verify that the connection is successful.

5. Click **Finish**.

6. In the DBeaver application, you should now see "Yugabyte - localhost" in the **Database Navigator** panel.

You can now expand the listing and see a listing of all databases available to the `yugabyte` user.

 ![Listing of databases](/images/develop/tools/dbeaver/dbeaver-list-of-databases.png)

## What's next

DBeaver has lots of features for developers and administrators to explore. For help using DBeaver, see the [DBeaver.io](https://dbeaver.io/) website and the [DBeaver documentation](https://github.com/dbeaver/dbeaver/wiki).

If you're looking for sample databases to explore YugabyteDB using DBeaver, see [Sample data](../../sample-data/).