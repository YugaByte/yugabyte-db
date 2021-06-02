---
title: Databricks
linkTitle: Databricks
description: Databricks
aliases:
menu:
  latest:
    identifier: databricks
    parent: ecosystem-integrations
    weight: 571
isTocNested: true
showAsideToc: true
---

[Databricks](https://www.databricks.com/) is a cloud-based data engineering tool used for processing and transforming massive quantities of data and exploring the data through machine learning models. Users can achieve the full potential of combining data, ELT processes, and machine learning in a cohesive context. Since YugabyteDB is API-compatible with both Cassandra and PostgreSQL, you can use Yugabyte's Cassandra-compatible [YCQL](../../../quick-start/explore/ycql/) or PostgreSQL-compatible [JDBC driver](../../../reference/drivers/yugabytedb-jdbc-driver/) to quickly get started with Databricks and Yugabyte.

The instructions on this page assume that **you have a Databricks cluster, and you want to connect it to Yugabyte using TLS** encryption in-transit.

### Tested Configuration

* Databricks Community Edition and Azure Databricks:
  * Runtime 7.4 (Scala 2.12, Spark 3.0.1)
* Yugabyte Enterprise 2.5.1 configured as follows:
  * GCP
  * Three `n1-standard-4` nodes 
  * A replication factor of three (RF=3)
  * 1 zone, and 3 availability zones
  * Encryption in-transit enabled
* OpenSSL version 2.8.3

### Connect your cluster

1. Do the following to download Yugabyte self-signed certificates from Yugabyte Platform to your local workstation:
    * Click Configs in the left-side navigation pane.
    * Click the Security tab, then click the Encryption In-Transit tab.
    * Use the Actions dropdown for the self-signed certificate chain to download the `YSQL Cert`, and the `Root CA Cert`.

    ![download page](/images/develop/ecosystem-integrations/databricks/download-certs.png)

1. To make the certificates available to Java, convert the client certificate to DER format:

    ```sh
    openssl x509 -in yugabytedb.crt -out yugabytedb.crt.der -outform der
    ```

1. Next, convert the client key files to DER format:

    ```sh
    openssl pkcs8 -topk8 -outform der -in yugabytedb.key -out yugabytedb.key.pk8 -nocrypt
    ```

1. Copy the following files to the Databricks DBFS certificate folder (`/dbfs/<path-to-certs>/`). (Refer to the [Databricks documentation](https://docs.databricks.com/data/databricks-file-system.html) for details.):

    * `yugabytedb.crt.der`
    * `yugabytedb.key.pk8`
    * `root.crt` 

1. In Databricks Scala notebook, set the following JDBC Driver options in either of the following ways:

    ```scala
    val jdbcDF = spark.read
      .format("jdbc")
      .option("url", "jdbc:postgresql://<ip or qualified url>:5433/<database>") 
      .option("dbtable", "<schema>.<table>")
      .option("user", "<username>")
      .option("ssl", "true")
      .option("sslmode", "verify-ca")
      .option("sslcert", "/dbfs/<path-to-certs>/yugabytedb.crt.der" )
      .option("sslkey", "/dbfs/<path-to-certs>/yugabytedb.key.pk8" )
      .option("sslrootcert", "/dbfs/<path-to-certs>/root.crt")
    ```

    <br/>
    OR

    ```scala
    val jdbcDF = spark.read
      .format("jdbc")
      .option("url", "jdbc:postgresql://<ip or qualified url>:5433/<database>") 
      .option("dbtable", "<schema>.<table>")
      .option("user", "<username>").option("ssl", "true")
      .option("sslmode", "require")
      .option("sslcert", "/dbfs/<path-to-certs>/yugabytedb.crt.der" )
      .option("sslkey", "/dbfs/<path-to-certs>/yugabytedb.key.pk8" )
    ```

## Sample app with output

1. Use the following YSQL script to load sample data into a test database:

    ```sql
    CREATE SCHEMA cycling;

    DROP TABLE IF EXISTS cycling.cyclist_name;

    CREATE TABLE cycling.cyclist_name (
      id bigint PRIMARY KEY,
      lastname character varying (50),
      firstname character varying (50)
    );

    INSERT INTO cycling.cyclist_name (id, lastname, firstname) 
      VALUES (1, 'VOS','Marianne');
    INSERT INTO cycling.cyclist_name (id, lastname, firstname) 
      VALUES (2, 'VAN DER BREGGEN','Anna');
    INSERT INTO cycling.cyclist_name (id, lastname, firstname) 
      VALUES (3, 'FRAME','Alex');
    INSERT INTO cycling.cyclist_name (id, lastname, firstname) 
      VALUES (4, 'TIRALONGO','Paolo');
    INSERT INTO cycling.cyclist_name (id, lastname, firstname) 
      VALUES (5, 'KRUIKSWIJK','Steven');
    INSERT INTO cycling.cyclist_name (id, lastname, firstname) 
      VALUES (6, 'MATTHEWS', 'Michael');
    ```

1. Create the corresponding Databricks Scala notebook:

    ```scala
    val jdbcDF = spark.read
      .format("jdbc")
      .option("url", "jdbc:postgresql://<your-ip-or-url>:5433/TEST") 
      .option("dbtable", "cycling.cyclist_name")
      .option("user", "yugabyte")
      .option("ssl", "true")
      .option("sslmode", "require")
      .option("sslcert", "/dbfs/<path-to-certs>/yugabytedb.crt.der" ) 
      .option("sslkey", "/dbfs/<path-to-certs>/yugabytedb.key.pk8" ) 
      .option("sslrootcert", "/<path-to-certs>/root.crt")
      .load()
    ```

1. Run the following Databricks command:

    ```scala
    jdbcDF.show()
    ```

    <br/>
    This should produce the following output:

    ```
    (1) Spark Jobs
    jdbcDF:org.apache.spark.sql.DataFrame
    id:long
    lastname:string
    firstname:string
    +---+---------------+---------+
    | id|       lastname|firstname|
    +---+---------------+---------+
    |  3|          FRAME|     Alex|
    |  5|     KRUIKSWIJK|   Steven|
    |  4|      TIRALONGO|    Paolo|
    |  2|VAN DER BREGGEN|     Anna|
    |  6|       MATTHEWS|  Michael|
    |  1|            VOS| Marianne|
    +---+---------------+---------+

    jdbcDF: org.apache.spark.sql.DataFrame = [id: bigint, lastname: string ... 1 more field]
    ```
