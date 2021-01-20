// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
package org.yb.loadtest;

import org.apache.spark.SparkConf;
import org.apache.spark.SparkContext;
import org.apache.spark.sql.Dataset;
import org.apache.spark.sql.Row;
import org.apache.spark.sql.SparkSession;

import org.junit.Test;
import org.junit.runner.RunWith;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.assertEquals;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.net.InetSocketAddress;
import java.util.Iterator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.stream.Collectors;

import org.yb.minicluster.BaseMiniClusterTest;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.util.YBTestRunnerNonTsanOnly;

import com.datastax.oss.driver.api.core.CqlSession;
import com.datastax.oss.driver.api.core.cql.ResultSet;
import com.datastax.spark.connector.cql.CassandraConnector;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestSpark3Jsonb extends BaseMiniClusterTest {

  private Logger logger = LoggerFactory.getLogger(TestSpark3Jsonb.class);
  private static String KEYSPACE = "test";
  private static String INPUT_TABLE = "person";
  private String phones[] = {
    "{\"code\":\"+42\",\"phone\":1000}",
    "{\"code\":\"+43\",\"phone\":1200}",
    "{\"code\":\"+44\",\"phone\":1400}"
  };
  private String phoneOut[] = {
    "+42",
    "+43",
    "+44"
  };

  @Test
  public void testJsonb() throws Exception {
      // Set up config.
      List<InetSocketAddress> addresses = miniCluster.getCQLContactPoints();

      // Setup the local spark master
      SparkConf conf = new SparkConf().setAppName("yb.spark-jsonb")
        .setMaster("local[1]")
        .set("spark.cassandra.connection.localDC", "datacenter1")
        .set("spark.cassandra.connection.host", addresses.get(0).getHostName())
        .set("spark.sql.catalog.mycatalog",
          "com.datastax.spark.connector.datasource.CassandraCatalog");

      CqlSession session = createTestSchemaIfNotExist(conf);

      SparkSession spark = SparkSession.builder().config(conf).getOrCreate();

      Dataset<org.apache.spark.sql.Row> df = spark.sqlContext().read()
          .format("org.apache.spark.sql.cassandra")
          .option("keyspace", KEYSPACE)
          .option("table", INPUT_TABLE).load();
      logger.info("Table loaded Sucessfully");

      df.createOrReplaceTempView("temp");

      spark.sqlContext().sql("select * from temp").show(false);
      logger.info("Data READ SuccessFully");

      Map<Integer, String> expectedValues = new HashMap<>();
      expectedValues.put(1, "Hammersmith London, UK");
      expectedValues.put(2, "Acton London, UK");
      expectedValues.put(3, "11 Acton London, UK");

      Dataset<Row> rows =
        spark.sql("SELECT id, address, phone, get_json_object(phone, '$.code') as phone " +
                    "FROM mycatalog.test.person " +
                    "WHERE get_json_object(phone, '$.phone') > 1000 order by id");

      Iterator<Row> iterator = rows.toLocalIterator();
      int rows_count = 1;
      while (iterator.hasNext()) {
          Row row = iterator.next();
          Integer id = row.getInt(0);
          String addr = row.getString(1);
          String jsonb = row.getString(3);
          assertEquals(jsonb, phoneOut[rows_count]);
          assertTrue(expectedValues.containsKey(id));
          assertEquals(expectedValues.get(id), addr);
          rows_count++;
      }
      assertEquals(3, rows_count);

      session.close();
      spark.close();
  }

  private CqlSession createTestSchemaIfNotExist(SparkConf conf) throws Exception {
      // Create the Cassandra connector to Spark.
      CassandraConnector connector = CassandraConnector.apply(conf);

      // Create a Cassandra session, and initialize the keyspace.
      CqlSession session = connector.openSession();

      String createKeyspace = "CREATE KEYSPACE IF NOT EXISTS " + KEYSPACE + ";";
      session.execute(createKeyspace);
      logger.info("Created keyspace name: {}", KEYSPACE);

      // Create table 'employee' if it does not exist.
      String tableWithKeysapce = KEYSPACE + "." + INPUT_TABLE;

      String createTable = "CREATE TABLE IF NOT EXISTS " + tableWithKeysapce
          + "(id int PRIMARY KEY,"
          +"name text,"
          +"address text,"
          +"phone jsonb"
          +");";

      session.execute(createTable);
      logger.info("Created table tablename: {}", INPUT_TABLE);

      // Insert a row.
      String insert = "INSERT INTO "+tableWithKeysapce
        + "(id, name, address,  phone) VALUES (1, 'John', 'Hammersmith London, UK','"
        + phones[0] + "');";
      session.execute(insert);
      insert = "INSERT INTO "+tableWithKeysapce
        +"(id, name, address,  phone) VALUES (2, 'Nick', 'Acton London, UK','" + phones[1] + "');";
      session.execute(insert);

      insert = "INSERT INTO "+tableWithKeysapce
        + "(id, name, address,  phone) VALUES (3, 'Smith', '11 Acton London, UK','"
        + phones[2] + "');";
      session.execute(insert);

      logger.info("Inserted data: {}" , insert);
      return session;
  }
}
