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

package com.yugabyte.sample.apps;

import java.nio.ByteBuffer;
import java.util.Arrays;
import java.util.List;

import com.datastax.oss.driver.api.core.CqlSession;
import com.datastax.oss.driver.api.core.cql.SimpleStatement;
import com.datastax.spark.connector.japi.rdd.CassandraJavaRDD;
import com.yugabyte.sample.common.CmdLineOpts;
import org.apache.commons.cli.CommandLine;
import org.apache.log4j.Logger;
import org.apache.spark.SparkConf;
import org.apache.spark.api.java.JavaPairRDD;
import org.apache.spark.api.java.JavaRDD;
import org.apache.spark.api.java.JavaSparkContext;
import scala.Tuple2;
import com.datastax.spark.connector.cql.CassandraConnector;
import scala.Tuple3;

import static com.datastax.spark.connector.japi.CassandraJavaUtil.javaFunctions;
import static com.datastax.spark.connector.japi.CassandraJavaUtil.mapTupleToRow;
import static com.datastax.spark.connector.japi.CassandraJavaUtil.someColumns;

public class CassandraSparkKeyValueCopy extends AppBase {

  private static final Logger LOG = Logger.getLogger(CassandraSparkKeyValueCopy.class);
  // Static initialization of this workload's config.
  static {
    // Set the app type to simple.
    appConfig.appType = AppConfig.Type.Simple;

    // Set Spark parallelism for this app.
    appConfig.numWriterThreads = 2;
  }

  // Input table.
  private String inputTableName;

  // Output table.
  private String outputTableName;

  // Default values.
  private static final String DEFAULT_INPUT_TABLE_NAME = "cassandrakeyvalue";
  private static final String DEFAULT_OUTPUT_TABLE_NAME = "cassandrakeyvaluecopy";

  // Names of command line arguments.
  private static final String INPUT_TABLE_ARG_NAME = "keyvaluecopy_input_table";
  private static final String OUTPUT_TABLE_ARG_NAME = "keyvaluecopy_output_table";

  private CqlSession session;
  private JavaSparkContext sc = null;

  public CqlSession getCqlSession() {
    return session;
  }

  @Override
  public void initialize(CmdLineOpts configuration) {
    // Setup the local spark master, with the desired parallelism.
    SparkConf conf = new SparkConf().setAppName("yb.kvcopy")
        .setMaster("local[" + appConfig.numWriterThreads + "]")
        .set("spark.cassandra.connection.localDC", "datacenter1")
        .set("spark.cassandra.connection.host", getRandomContactPoint().getHost());

    // Create the Java Spark context object.
    sc = new JavaSparkContext(conf);

    // Create the Cassandra connector to Spark.
    CassandraConnector connector = CassandraConnector.apply(conf);

    // Create a Cassandra session
    session = connector.openSession();

    CommandLine commandLine = configuration.getCommandLine();

    //--------------------- Setting Input source (Cassandra table or file) -------------------------
    if (commandLine.hasOption(INPUT_TABLE_ARG_NAME)) {
      inputTableName = commandLine.getOptionValue(INPUT_TABLE_ARG_NAME);
      LOG.info("Using " + INPUT_TABLE_ARG_NAME + " := " + inputTableName);
    } else {
      inputTableName = DEFAULT_INPUT_TABLE_NAME;
      LOG.info("No input table given, use the default value: " + DEFAULT_INPUT_TABLE_NAME);
    }

    //----------------------- Setting Output location (Cassandra table) ----------------------------
    if (commandLine.hasOption(OUTPUT_TABLE_ARG_NAME)) {
      outputTableName = commandLine.getOptionValue(OUTPUT_TABLE_ARG_NAME);
      LOG.info("Using " + OUTPUT_TABLE_ARG_NAME + ": " + outputTableName);
    } else { // defaults
      outputTableName = DEFAULT_OUTPUT_TABLE_NAME;
      LOG.info("No output table given, use the default value: " + DEFAULT_OUTPUT_TABLE_NAME);
    }
  }

  @Override
  public void run() {
    StringBuilder sb = new StringBuilder();
    boolean first = true;
    for (CmdLineOpts.ContactPoint contactPoint : configuration.getContactPoints()) {
      if (!first) {
        sb.append(",");
      } else {
        first = false;
      }
      sb.append(contactPoint.getHost());
    }

    String nodes = sb.toString();

    //------------------------------------------- Input ------------------------------------------
    JavaRDD<Tuple3<String, ByteBuffer, String>> rows;
    // Read rows from table and convert them to an RDD.
    rows = javaFunctions(sc).cassandraTable(getKeyspace(), inputTableName).select("k", "v1", "v2")
        .map(row -> new Tuple3<>(row.getString("k"),
                                 row.getBytes("v1"),
                                 row.getString("v2")));

    //------------------------------------------- Output -----------------------------------------
    String outTable = getKeyspace() + "." + outputTableName;

    // Drop the output table if it already exists.
    String drop_stmt = String.format("DROP TABLE IF EXISTS %s;", outTable);
    session.execute(drop_stmt);

    // Create the output table.
    session.execute("CREATE TABLE IF NOT EXISTS " + outTable +
        " (k VARCHAR PRIMARY KEY, v1 BLOB, v2 jsonb)");

    // Save the output to the CQL table.
    javaFunctions(rows).writerBuilder(getKeyspace(),
        outputTableName,
        mapTupleToRow(String.class, ByteBuffer.class, String.class))
        .withColumnSelector(someColumns("k", "v1", "v2"))
        .saveToCassandra();
  }

  @Override
  public List<String> getWorkloadDescription() {
    return Arrays.asList(
      "Simple Spark app that reads from an input table and writes to an output table.",
      "It does no additional processing of the data, only tests reading/writing data.",
      "Use num_threads_write option to set the number of Spark worker threads.",
      "Input source an input_table which defaults to '" + DEFAULT_INPUT_TABLE_NAME + "'");
  }

  @Override
  public List<String> getWorkloadOptionalArguments() {
    return Arrays.asList(
      "--num_threads_write " + appConfig.numWriterThreads,
      "--" + INPUT_TABLE_ARG_NAME + " " + DEFAULT_INPUT_TABLE_NAME,
      "--" + OUTPUT_TABLE_ARG_NAME + " " + DEFAULT_OUTPUT_TABLE_NAME);
  }
}
