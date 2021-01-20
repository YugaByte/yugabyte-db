---
title: Extensions [YSQL]
headerTitle: Install and use extensions
linkTitle: Extensions
description: Install and use extensions with PostgreSQL-compatible YSQL.
summary: Reference for YSQL extensions
block_indexing: true
menu:
  stable:
    identifier: api-ysql-extensions
    parent: api-ysql
    weight: 4400
isTocNested: true
showAsideToc: true
---

This page documents how to install and use PostgreSQL extensions that are tested to work with YSQL. Note that since YugabyteDB’s storage architecture is not the same as that of native PostgreSQL, PostgreSQL extensions, especially those that interact with the storage layer, are not expected to work as-is on YugabyteDB. We intend to incrementally develop support for as many extensions as possible.

## Pre-bundled extensions

These are extensions that are included in the standard YugabyteDB distribution and can be enabled in YSQL by running the [`CREATE EXTENSION`](../commands/ddl_create_extension) statement.

### fuzzystrmatch

The `fuzzystrmatch` extension provides several functions to determine similarities and distance between strings.

#### Example

```plpgsql
CREATE EXTENSION fuzzystrmatch;
SELECT levenshtein('Yugabyte', 'yugabyte'), metaphone('yugabyte', 8);
 levenshtein | metaphone
-------------+-----------
           2 | YKBT
(1 row)
```

For more information see [`fuzzystrmatch`](https://www.postgresql.org/docs/11/fuzzystrmatch.html) in the PostgreSQL Docs.

### pgcrypto

The `pgcrypto` extension provides various cryptographic functions.

#### Example

```plpgsql
CREATE EXTENSION pgcrypto;
CREATE TABLE pgcrypto_example(id uuid PRIMARY KEY DEFAULT gen_random_uuid(), content text, digest text);
INSERT INTO pgcrypto_example (content, digest) values ('abc', digest('abc', 'sha1'));
SELECT * FROM pgcrypto_example;
                  id                  | content |                   digest
--------------------------------------+---------+--------------------------------------------
 b8f2e2f7-0b8d-4d26-8902-fa4f5277869d | abc     | \xa9993e364706816aba3e25717850c26c9cd0d89d
(1 row)
```

For more information see [`pgcrypto`](https://www.postgresql.org/docs/current/pgcrypto.html) in the PostgreSQL Docs.

### pg_stat_statements

The [`pg_stat_statements`](https://www.postgresql.org/docs/11/pgstatstatements.html) extension module is installed by default, but must be enabled before the `pg_stat_statements` view can be queried. 

```plpgsql
CREATE EXTENSION pg_stat_statements;
SELECT query, calls, total_time, min_time, max_time, mean_time, stddev_time, rows FROM pg_stat_statements;
```

You can also access the output of `pg_stat_statements` (in JSON format) by using a web browser to go to `https://<yb-tserver-ip>:13000/statements`, where `<yb-tserver-ip>` is the IP address of any YB-TServer node of your cluster.

For more information, see [`pg_stat_statements`](https://www.postgresql.org/docs/11/pgstatstatements.html) in the PostgreSQL documentation.

### Server Programming Interface (spi) module

The `spi` module lets developers use the [Server Programming Interface (SPI)](https://www.postgresql.org/docs/11/spi.html) to use the C programming language to create user-defined functions and stored procedures and to run YSQL queries directly against YugabyteDB. In YugabyteDB, the following four (of the five) extensions provided in the `spi` module can be used:

- `insert_username`: Functions for tracking who changed a table.
- `moddatetime`: Functions for tracking last modification time
- `autoinc`: Functions for autoincrementing fields.
- `refint`: Functions for implementing referential integrity.

#### Example

1. Set up a table with triggers for tracking modification time and user (role).
    Connect with `ysqlsh` and run the commands below.

```plpgsql
CREATE EXTENSION insert_username;
CREATE EXTENSION moddatetime;

CREATE TABLE spi_test (
  id int primary key,
  content text,
  username text not null,
  moddate timestamp DEFAULT CURRENT_TIMESTAMP NOT NULL
);

CREATE TRIGGER insert_usernames
  BEFORE INSERT OR UPDATE ON spi_test
  FOR EACH ROW
  EXECUTE PROCEDURE insert_username (username);

CREATE TRIGGER update_moddatetime
  BEFORE UPDATE ON spi_test
  FOR EACH ROW
  EXECUTE PROCEDURE moddatetime (moddate);
```

2. Insert some rows. Each insert should add the current role as `username` and the current timestamp as `moddate`.

```plpgsql
SET ROLE yugabyte;
INSERT INTO spi_test VALUES(1, 'desc1');

SET ROLE postgres;
INSERT INTO spi_test VALUES(2, 'desc2');
INSERT INTO spi_test VALUES(3, 'desc3');

SET ROLE yugabyte;
INSERT INTO spi_test VALUES(4, 'desc4');

SELECT * FROM spi_test ORDER BY id;
 id | content | username |          moddate
----+---------+----------+----------------------------
  1 | desc1   | yugabyte | 2019-09-13 16:55:53.969907
  2 | desc2   | postgres | 2019-09-13 16:55:53.983306
  3 | desc3   | postgres | 2019-09-13 16:55:53.98658
  4 | desc4   | yugabyte | 2019-09-13 16:55:53.991315
(4 rows)
```

{{< note title="Note" >}}

YSQL should have users `yugabyte` and (for compatibility) `postgres`, which are created by default.

{{< /note >}}

3. Update some rows. Should update both `username`  and `moddate` accordingly.

```plpgsql
UPDATE spi_test SET content = 'desc1_updated' WHERE id = 1;
UPDATE spi_test SET content = 'desc3_updated' WHERE id = 3;
SELECT * FROM spi_test ORDER BY id;
 id |    content    | username |          moddate
----+---------------+----------+----------------------------
  1 | desc1_updated | yugabyte | 2019-09-13 16:56:27.623513
  2 | desc2         | postgres | 2019-09-13 16:55:53.983306
  3 | desc3_updated | yugabyte | 2019-09-13 16:56:27.634099
  4 | desc4         | yugabyte | 2019-09-13 16:55:53.991315
(4 rows)
```

For more information, see [`spi module`](https://www.postgresql.org/docs/current/contrib-spi.html) in the PostgreSQL Docs.

### tablefunc

The `tablefunc` extension provides several table functions. For example, `normal_rand()` creates values, picked using a pseudorandom generator, from an ideal normal distribution. You specify how many values you want, and the mean and standard deviation of the ideal distribution. You use it in the same way that you use `generate_series()`

#### Example

```plpgsql
CREATE EXTENSION tablefunc;

CREATE TABLE T(k int primary key, v double precision);

PREPARE insert_k_v_pairs(int) AS
INSERT INTO t(k, v)
SELECT
  generate_series(1, $1),
  normal_rand($1, 1000.0, 10.0);
```
Test it like this
```plpgsql
DELETE FROM t;
EXECUTE insert_k_v_pairs(10);
SELECT k, to_char(v, '9999.99') AS v
FROM t
ORDER BY k;
```
You'll see a result like this:
```
 k  |    v     
----+----------
  1 |   988.53
  2 |  1005.18
  3 |  1014.30
  4 |  1000.92
  5 |   999.51
  6 |  1000.94
  7 |  1007.45
  8 |   991.22
  9 |   987.95
 10 |   996.57
```
Each time you repeat the test, you'll see different generated values for _"v"_. The section [Analyzing a normal distribution with percent_rank(), cume_dist() and ntile()](../exprs/window_functions/analyzing-a-normal-distribution/), within the major documentation section [Window functions](../exprs/window_functions/), uses `normal_rand()`. It populates a table with a large number, say 100,000, rows and displays the outcome as a histogram that clearly shows the familiar bell-curve shape.

These other functions are brought by `tablefunc`: `connectby()`; and `crosstab()`and  `crosstabN()`.

The `connectby()` function displays a hierarchy of the kind that you see in an _"employees"_ table with a reflexive foreign key constraint where _"manager_id"_ refers to _"employee_id"_. Each next deeper level in the tree is indented from its parent following the well-known pattern.

The `crosstab()`and  `crosstabN()` functions produce “pivot” displays. The _"N"_ in crosstabN() indicates the fact that a few, `crosstab1()', `crosstab2()', `crosstab3()`, are provided natively by the extension and that you can follow documented steps to create more.

For more information see [`tablefunc`](https://www.postgresql.org/docs/11/tablefunc.html) in the PostgreSQL Docs.

## Extensions requiring installation

Other extensions have to be installed manually before they can be enabled with the [`CREATE EXTENSION`](../commands/ddl_create_extension) statement.

{{< note title="Note" >}}

Currently, in a multi-node setup, the installation instructions below must be done on _every_ node in the cluster.

{{< /note >}}

Typically, extensions need three types of files:

- Shared library files (`<name>.so`)
- SQL files (`<name>--<version>.sql`)
- Control files (`<name>.control`)

In order to install an extension, you need to copy these files into the respective directories of your YugabyteDB installation.

Shared library files will be in the `pkglibdir` directory, while SQL and control files should be in the `extension` subdirectory of the `libdir` directory.
To find these directories on your local installation, you can use the YugabyteDB `pg_config` executable.
First, alias it to `yb_pg_config` by replacing `<yugabyte-path>` with the path to your YugabyteDB installation in the command below and then running it.  

```sh
$ alias yb_pg_config=/<yugabyte-path>/postgres/bin/pg_config
```

Now you can list existing shared libraries with:

```sh
$ ls "$(yb_pg_config --pkglibdir)"
```

And SQL and control files for already-installed extensions with:

```sh
$ ls "$(yb_pg_config --sharedir)"/extension/
```

To obtain these files for your target extension, you can build it from scratch following the extension's build instructions.
Alternatively, if you already have PostgreSQL (ideally version `11.2` for best YSQL compatibility) with that extension installed, then you can find these files as follows:

```sh
$ ls "$(pg_config --pkglibdir)" | grep <name>
```

```sh
$ ls "$(pg_config --sharedir)"/extension/ | grep <name>
```

Copy those files to the YugabyteDB installation.
Restart the cluster (or the respective node in a multi-node install).
Finally, connect to the cluster with `ysqlsh` and run the `CREATE EXTENSION` statement to create the extension.

{{< note title="Note" >}}

Only some extensions are currently supported.
If you encounter any issues when installing or using a particular extension, file a GitHub issue in the [yugabyte/yugabyte-db](https://github.com/yugabyte/yugabyte-db/issues) repository.

{{< /note >}}

### PostGIS

[PostGIS](https://postgis.net/) is a spatial database extender for PostgreSQL-compatible object-relational databases.
The simplest way to set it up locally is to install it together with regular PostgreSQL.

For instance, on macOS, you can:

- download and install [Postgres.app](https://postgresapp.com/)
- install with Homebrew:

    ```sh
    $ brew install postgres && brew install postgis
    ```

Now follow the instructions described above to copy the needed files into your YugabyteDB installation, and then create
the extension.

```sh
$ cp -v "$(pg_config --pkglibdir)"/*postgis*.so "$(yb_pg_config --pkglibdir)" && 
  cp -v "$(pg_config --sharedir)"/extension/*postgis*.sql "$(yb_pg_config --sharedir)"/extension && 
  cp -v "$(pg_config --sharedir)"/extension/*postgis*.control "$(yb_pg_config --sharedir)"/extension &&
  ./bin/ysqlsh -c "CREATE EXTENSION postgis";
```

This might take a couple of minutes.

#### Example

1. Get a sample [postgis dataset](https://data.edmonton.ca/Geospatial-Boundaries/City-of-Edmonton-Neighbourhood-Boundaries/jfvj-x253):

```sh
$ wget -O edmonton.zip "https://data.edmonton.ca/api/geospatial/jfvj-x253?method=export&format=Shapefile" && unzip edmonton.zip
```

2. Extract the dataset using the `shp2pgsql` tool.
    This should come with your PostgreSQL installation — it is not yet packaged with YSQL.

```sh
$ shp2pgsql geo_export_*.shp > edmonton.sql
```

3. Edit the generated `edmonton.sql` for YSQL compatibility.
    First, inline the `PRIMARY KEY` declaration for `gid` as YSQL does not yet support adding primary key constraints after the table creation.
    Additionally, for simplicity, change the table name (and references to it in the associated `INSERT`s) to just `geo_export` (i.e. remove the UUID postfix).
    The `edmonton.sql` file should now start as follows:

```
SET CLIENT_ENCODING TO UTF8;
SET STANDARD_CONFORMING_STRINGS TO ON;
BEGIN;
CREATE TABLE "geo_export" (gid serial PRIMARY KEY,
  "area_km2" numeric,
  "name" varchar(254),
  "number" numeric);
SELECT AddGeometryColumn('','geo_export','geom','0','MULTIPOLYGON',2);

INSERT INTO "geo_export" ("area_km2","name","number",geom) VALUES ...
```

4. Load the sample data.

```sh
$ ./bin/ysqlsh -a -f edmonton.sql
```

5. Run some sample queries. Connect with `ysqlsh` and run:

```plpgsql
SELECT name, area_km2, ST_Area(geom), ST_Area(geom)/area_km2 AS area_ratio FROM "geo_export" LIMIT 10;
            name            |     area_km2      |       st_area        |      area_ratio
----------------------------+-------------------+----------------------+----------------------
 River Valley Terwillegar   | 3.077820277027079 | 0.000416617423004673 | 0.000135361192501822
 Carleton Square Industrial | 0.410191631391664 | 5.56435079305678e-05 | 0.000135652469899947
 Cy Becker                  | 1.015144841249301 | 0.000137900847258255 | 0.000135843518732308
 Elsinore                   | 0.841471068786406 | 0.000114331091817771 |  0.00013587049639468
 McLeod                     | 0.966538217483227 | 0.000131230296771637 | 0.000135773520796051
 Gainer Industrial          | 0.342464541730177 | 4.63954326887451e-05 | 0.000135475142782225
 Coronet Industrial         | 1.606907195063447 | 0.000217576340986435 | 0.000135400688760899
 Marquis                    | 9.979100854886905 |  0.00135608901739072 | 0.000135892906295924
 South Terwillegar          | 1.742840325820606 | 0.000235695089933611 | 0.000135236192576985
 Carlisle                   | 0.961897333826841 | 0.000130580966739925 | 0.000135753538499185
(10 rows)

SELECT a.name, b.name FROM "geo_export" AS a, "geo_export" AS b
WHERE ST_Intersects(a.geom, b.geom) AND a.name LIKE 'University of Alberta';
         name          |          name
-----------------------+-------------------------
 University of Alberta | University of Alberta
 University of Alberta | McKernan
 University of Alberta | Belgravia
 University of Alberta | Garneau
 University of Alberta | River Valley Mayfair
 University of Alberta | River Valley Walterdale
 University of Alberta | Windsor Park
(7 rows)
```

{{< note title="Note" >}}

YSQL does not yet support GiST indexes. This is tracked in [GitHub issue #1337](https://github.com/yugabyte/yugabyte-db/issues/1337).

{{< /note >}}

### postgresql-hll (PostgreSQL extension for HyperLogLog)

The [`postgresql-hll`](https://github.com/citusdata/postgresql-hll) module introduces a new data type `hll`, which is a HyperLogLog data structure.
HyperLogLog is a fixed-size, set-like structure used for distinct value counting with tunable precision.

The first step is to install `postgres-hll` [from source](https://github.com/citusdata/postgresql-hll#from-source) locally in a PostgreSQL instance.
It is best to use the same PostgreSQL version that is incorporated into YugabyteDB. You can see the PostgreSQL version incorporated in YugabyteDB installation by using the following `ysqlsh` command:

```sh
$ ./bin/ysqlsh --version
psql (PostgreSQL) 11.2-YB-2.1.2.0-b0
```

Above you performed the steps in your PostgreSQL 11.2 instance. After installing the extension there, now copy the files to your YugabyteDB instance:

```sh
$ cp -v "$(pg_config --pkglibdir)"/*hll*.so "$(yb_pg_config --pkglibdir)" && 
  cp -v "$(pg_config --sharedir)"/extension/*hll*.sql "$(yb_pg_config --sharedir)"/extension && 
  cp -v "$(pg_config --sharedir)"/extension/*hll*.control "$(yb_pg_config --sharedir)"/extension &&
  ./bin/ysqlsh -c "CREATE EXTENSION \"hll\"";
```

#### Example

You can run a quick example for the [postgresql-hll](https://github.com/citusdata/postgresql-hll#usage) repository.
Connect with `ysqlsh` and run:

```plpgsql
yugabyte=# CREATE TABLE helloworld (id integer, set hll);
CREATE TABLE
--- Insert an empty HLL
yugabyte=# INSERT INTO helloworld(id, set) VALUES (1, hll_empty());
INSERT 0 1
--- Add a hashed integer to the HLL
yugabyte=# UPDATE helloworld SET set = hll_add(set, hll_hash_integer(12345)) WHERE id = 1;
UPDATE 1
--- Or add a hashed string to the HLL
yugabyte=# UPDATE helloworld SET set = hll_add(set, hll_hash_text('hello world')) WHERE id = 1;
UPDATE 1
--- Get the cardinality of the HLL
yugabyte=# SELECT hll_cardinality(set) FROM helloworld WHERE id = 1;
 hll_cardinality 
-----------------
               2
(1 row)
```

### uuid-ossp

The [`uuid-ossp`](https://www.postgresql.org/docs/current/uuid-ossp.html) extension provides functions to generate
universally unique identifiers (UUIDs) and also functions to produce certain special UUID constants.

The easiest way to install it is to copy the files from an existing PostgreSQL installation into Yugabyte, and then create the extension.

```sh
$ cp -v "$(pg_config --pkglibdir)"/*uuid-ossp*.so "$(yb_pg_config --pkglibdir)" && 
  cp -v "$(pg_config --sharedir)"/extension/*uuid-ossp*.sql "$(yb_pg_config --sharedir)"/extension && 
  cp -v "$(pg_config --sharedir)"/extension/*uuid-ossp*.control "$(yb_pg_config --sharedir)"/extension &&
  ./bin/ysqlsh -c "CREATE EXTENSION \"uuid-ossp\"";
```

#### Example

Connect with `ysqlsh` and run:

```plpgsql
SELECT uuid_generate_v1(), uuid_generate_v4(), uuid_nil();
           uuid_generate_v1           |           uuid_generate_v4           |               uuid_nil
--------------------------------------+--------------------------------------+--------------------------------------
 69975ce4-d827-11e9-b860-bf2e5a7e1380 | 088a9b6c-46d8-4276-852b-64908b06a503 | 00000000-0000-0000-0000-000000000000
(1 row)
```
