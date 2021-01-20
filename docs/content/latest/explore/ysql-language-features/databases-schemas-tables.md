---
title: Schemas and Tables
linkTitle: Schemas and Tables
description: Schemas and Tables in YSQL
headcontent: Schemas and Tables
image: /images/section_icons/secure/create-roles.png
menu:
  latest:
    identifier: explore-ysql-language-features-databases-schemas-tables
    parent: explore-ysql-language-features
    weight: 100
isTocNested: true
showAsideToc: true
---

This section covers basic topics such as the YSQL shell `ysqlsh`, databases, schemas and tables.

## `ysqlsh` SQL shell

The recommended command line shell to interact with a Yugabyte SQL database cluster is using the `ysqlsh` shell utility.

{{< tip title="Tip" >}}
All the `psql` commands just work as is in `ysqlsh`. This is because the `ysqlsh` shell is in turn derived from PostgreSQL shell `psql` code base - the default settings such as the database default port and the output format of some of the schema commands have been modified for YugabyteDB.
{{< /tip >}}

Connect to any node of the database cluster as shown below:

```sh
$ ./bin/ysqlsh -h 127.0.0.1
```

This should bring up the following prompt, which prints the version of `ysqlsh` being used.

```
ysqlsh (11.2-YB-2.5.0.0-b0)
Type "help" for help.

yugabyte=#
```

You can check the version of the database server by running the following.

```sql
yugabyte=# SELECT version();
```

The output shows that the YugabyteDB server version is 2.5.0.0-b0, and is a fork of PostgreSQL v11.2:
```
                                              version
----------------------------------------------------------------------------------------------------------
 PostgreSQL 11.2-YB-2.5.0.0-b0 on x86_64-<os, compiler version, etc>, 64-bit
(1 row)
```

### Query timing

By default the timing of query results will be displayed in milliseconds in `ysqlsh`. You can toggle this off (and on again) by using the `\timing` command.

```
yugabyte=# \timing
Timing is off.
```

### List all databases

List all databases using the following statements.
```sql
yugabyte=# \l
```
Output:
```
                                   List of databases
      Name       |  Owner   | Encoding | Collate |    Ctype    |   Access privileges
-----------------+----------+----------+---------+-------------+-----------------------
 postgres        | postgres | UTF8     | C       | en_US.UTF-8 |
 system_platform | postgres | UTF8     | C       | en_US.UTF-8 |
 template0       | postgres | UTF8     | C       | en_US.UTF-8 | =c/postgres          +
                 |          |          |         |             | postgres=CTc/postgres
 template1       | postgres | UTF8     | C       | en_US.UTF-8 | =c/postgres          +
                 |          |          |         |             | postgres=CTc/postgres
 yugabyte        | postgres | UTF8     | C       | en_US.UTF-8 |
(5 rows)
```

### List all schemas

You can list all schemas using `\dn` as shown here.
```sql
yugabyte=# \dn
```
Example:
```
# Create an example schema.
yugabyte=# create schema example;
CREATE SCHEMA

# List all schemas.
yugabyte=# \dn
  List of schemas
  Name   |  Owner
---------+----------
 example | yugabyte
 public  | postgres
```

### List tables

You can list all the tables in a database by using the `\d` command.
```sql
yugabyte=# \d
```
Output:
```
                 List of relations
 Schema |        Name         |   Type   |  Owner
--------+---------------------+----------+----------
 public | users               | table    | yugabyte
 public | users_id_seq        | sequence | yugabyte
 ```

### Describe a table

Describe a table using the `\d` command.
```sql
yugabyte=# \d users
```
Output:
```
                                Table "public.users"
  Column  |     Type      | Collation | Nullable |              Default
----------+---------------+-----------+----------+-----------------------------------
 id       | integer       |           | not null | nextval('users_id_seq'::regclass)
 username | character(25) |           | not null |
 enabled  | boolean       |           |          | true
Indexes:
    "users_pkey" PRIMARY KEY, lsm (id HASH)
```

## Users

YugabyteDB has two admin users already created - `yugabyte` (the recommended user) and `postgres` (mainly for backward compatibility with PostgreSQL). You can check this as shown below.

```sql
yugabyte=# \conninfo
```
This should print an output that looks as follows: `You are connected to database "yugabyte" as user "yugabyte" on host "127.0.0.1" at port "5433"`.

To check all the users provisioned, run:
```sql
yugabyte=# \du
```
```
                                   List of roles
 Role name |                         Attributes                         | Member of
-----------+------------------------------------------------------------+-----------
 postgres  | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
 yugabyte  | Superuser, Create role, Create DB, Replication, Bypass RLS | {}
```

## Databases

YSQL supports databases and schemas, much like PostgreSQL.

To create a new database `testdb`, run the following statement:
```sql
CREATE DATABASE testdb;
```

To list all databases, use the `\l` command.

```
yugabyte=# \l
                                   List of databases
      Name       |  Owner   | Encoding | Collate |    Ctype    |   Access privileges
-----------------+----------+----------+---------+-------------+-----------------------
 postgres        | postgres | UTF8     | C       | en_US.UTF-8 |
 system_platform | postgres | UTF8     | C       | en_US.UTF-8 |
 template0       | postgres | UTF8     | C       | en_US.UTF-8 | =c/postgres          +
                 |          |          |         |             | postgres=CTc/postgres
 template1       | postgres | UTF8     | C       | en_US.UTF-8 | =c/postgres          +
                 |          |          |         |             | postgres=CTc/postgres
 testdb          | yugabyte | UTF8     | C       | en_US.UTF-8 |
 yugabyte        | postgres | UTF8     | C       | en_US.UTF-8 |
(6 rows)
```

To connect to this database, use the `\c` command as shown below.

```sql
\c testdb
```

You should see the following output.
```
You are now connected to database "testdb" as user "yugabyte".
```

To drop the database we just created, simply connect to another database and use the `DROP` command.

```
# First connect to another database.
testdb=# \c yugabyte
You are now connected to database "yugabyte" as user "yugabyte".

# Next use the DROP command.
yugabyte=# drop database testdb;
DROP DATABASE

# Verify the database is no longer present.
yugabyte=# \l
                                   List of databases
      Name       |  Owner   | Encoding | Collate |    Ctype    |   Access privileges
-----------------+----------+----------+---------+-------------+-----------------------
 postgres        | postgres | UTF8     | C       | en_US.UTF-8 |
 system_platform | postgres | UTF8     | C       | en_US.UTF-8 |
 template0       | postgres | UTF8     | C       | en_US.UTF-8 | =c/postgres          +
                 |          |          |         |             | postgres=CTc/postgres
 template1       | postgres | UTF8     | C       | en_US.UTF-8 | =c/postgres          +
                 |          |          |         |             | postgres=CTc/postgres
 yugabyte        | postgres | UTF8     | C       | en_US.UTF-8 |
```

## Tables

Let's create a simple table as shown below.

```sql
CREATE TABLE users (
  id serial,
  username CHAR(25) NOT NULL,
  enabled boolean DEFAULT TRUE,
  PRIMARY KEY (id)
  );
```

To list all tables, use the `\dt` command.
```sql
\dt
```
Output:
```
yugabyte=# \dt
                List of relations
 Schema |        Name         | Type  |  Owner
--------+---------------------+-------+----------
 public | users               | table | yugabyte
```

To list the table and the sequence we created, use the `\d` command.
```
 Schema |        Name         |   Type   |  Owner
--------+---------------------+----------+----------
 public | users               | table    | yugabyte
 public | users_id_seq        | sequence | yugabyte
```

To describe the table we just created, do the following.
```sql
\d users
```
Output:
```
yugabyte=# \d users
                                Table "public.users"
  Column  |     Type      | Collation | Nullable |              Default
----------+---------------+-----------+----------+-----------------------------------
 id       | integer       |           | not null | nextval('users_id_seq'::regclass)
 username | character(25) |           | not null |
 enabled  | boolean       |           |          | true
Indexes:
    "users_pkey" PRIMARY KEY, lsm (id HASH)
```


## Schemas

A schema is a named collection of tables, views, indexes, sequences, data types, operators, and functions. 

To create the schema with name `myschema`, run the following command.
```sql
testdb=# create schema myschema;
```

To create a table in this schema, run the following:

```sql
testdb=# create table myschema.company(
   ID   INT              NOT NULL,
   NAME VARCHAR (20)     NOT NULL,
   AGE  INT              NOT NULL,
   ADDRESS  CHAR (25),
   SALARY   DECIMAL (18, 2),
   PRIMARY KEY (ID)
);
```

Note that at this point, the `default` schema is still the selected schema, and running the `\d` command would not list the table we just created.

To see which schema is currently the default, run the following.

```sql
SHOW search_path;
```
You should see the following output.
```
testdb=# SHOW search_path;
   search_path
-----------------
 "$user", public
(1 row)
```

To set `myschema` as the default schema in this session, do the following.

```sql
SET search_path=myschema;
```
Now, we should be able to list the table we created.

```
testdb=# SHOW search_path;
 search_path
-------------
 myschema
(1 row)

testdb=# \d
           List of relations
  Schema  |  Name   | Type  |  Owner
----------+---------+-------+----------
 myschema | company | table | yugabyte
(1 row)
```

To drop the schema `myschema` and all the objects inside it, first change the current default schema.

```sql
SET search_path=default;
```

Next, run the `DROP` statement as shown below.

```sql
DROP SCHEMA myschema CASCADE;
```

You should see the following output.

```
testdb=# DROP SCHEMA myschema CASCADE;
NOTICE:  00000: drop cascades to table myschema.company
LOCATION:  reportDependentObjects, dependency.c:1004
DROP SCHEMA
```



