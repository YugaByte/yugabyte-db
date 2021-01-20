---
title: Northwind sample database
headerTitle: Northwind sample database
linkTitle: Northwind
description: Use the Northwind sample database to explore and learn YugabyteDB.
block_indexing: true
menu:
  stable:
    identifier: northwind
    parent: sample-data
    weight: 2752
isTocNested: true
showAsideToc: true
---

Download and install the PostgreSQL-compatible version of the Northwind dataset on the YugabyteDB distributed SQL database.

## About the Northwind sample database

The Northwind database is a sample database that was originally created by Microsoft and used as the basis for their tutorials in a variety of database products for decades. The Northwind database contains the sales data for a fictitious company called “Northwind Traders,” which imports and exports specialty foods from around the world. The Northwind database is an excellent tutorial schema for a small-business ERP, with customers, orders, inventory, purchasing, suppliers, shipping, employees, and single-entry accounting. The Northwind database has since been ported to a variety of non-Microsoft databases, including PostgreSQL.

The Northwind dataset includes sample data for the following.

- **Suppliers**: Suppliers and vendors of Northwind
- **Customers**: Customers who buy products from Northwind
- **Employees**: Employee details of Northwind traders
- **Products**: Product information
- **Shippers**: The details of the shippers who ship the products from the traders to the end-customers
- **Orders and Order_Details**: Sales Order transactions taking place between the customers & the company

The Northwind sample database includes 14 tables and the table relationships are showcased in the following entity relationship diagram.

![Northwind ER diagram](/images/sample-data/northwind/northwind-er-diagram.png)

## Install the Northwind sample database

Follow the steps here to download and install the Northwind sample database.

### Before you begin

To use the Northwind sample database, you must have installed and configured YugabyteDB. To get up and running quickly, see [Quick Start](/stable/quick-start/).

### 1. Download the SQL scripts

You can download the Northwind database files, which are compatible with YugabyteDB, from the [`sample` directory of the YugabyteDB GitHub repository](https://github.com/yugabyte/yugabyte-db/tree/master/sample). Here are the two files you’ll need.

- [northwind_ddl.sql](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/northwind_ddl.sql) — Creates tables and other database objects
- [northwind_data.sql](https://raw.githubusercontent.com/yugabyte/yugabyte-db/master/sample/northwind_data.sql) — Loads the sample data

### 2. Open the YSQL shell

To open the YSQL shell, run the `ysqlsh` command from the YugabyteDB root directory.

```sh
$ ./bin/ysqlsh
```

```
ysqlsh (11.2)
Type "help" for help.
yugabyte=#
```

### 3. Create the Northwind database

To create the Northwind database, run the following `CREATE DATABASE` statement.

```plpgsql
CREATE DATABASE northwind;
```

Confirm that you have the Northwind database by listing out the databases on your cluster.

```plpgsql
yugabyte=# \l
```

![Northwind list of databases](/images/sample-data/northwind/northwind-list-of-dbs.png)

Connect to the Northwind database.

```plpgsql
yugabyte=# \c northwind
```

```
You are now connected to database "northwind" as user "yugabyte".
northwind=#
```

### 4. Build the tables and objects

To build the tables and database objects, execute the `northwind_ddl.sql` SQL script.

```plpgsql
northwind=# \i share/northwind_ddl.sql
```

You can verify that all 14 tables have been created by running the `\d` command.

```plpgsql
northwind=# \d
```

![Northwind list of relations](/images/sample-data/northwind/northwind-list-of-relations.png)

### 5. Load the sample data

To load the `northwind` database with sample data, run the `\i` command to execute commands in the `northwind_data.sql` file.

```plpgsql
northwind=# \i share/northwind_data.sql
```

To verify that you have some data to work with, you can run a simple SELECT statement to pull data from the `customers` table.

```plpgsql
northwind=# SELECT * FROM customers LIMIT 2;
```

## Explore the Northwind database

That’s it! You are now ready to start exploring the Northwind database and YugabyteDB features using the command line or your favorite PostgreSQL tool.
