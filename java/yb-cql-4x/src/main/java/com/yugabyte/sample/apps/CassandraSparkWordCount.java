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

import java.util.Arrays;
import java.util.List;

import com.datastax.oss.driver.api.core.CqlSession;
import com.datastax.oss.driver.api.core.type.DataTypes;
import com.datastax.oss.driver.api.querybuilder.SchemaBuilder;
import com.yugabyte.sample.common.CmdLineOpts;
import org.apache.commons.cli.CommandLine;
import org.apache.log4j.Logger;
import org.apache.spark.SparkConf;
import org.apache.spark.api.java.JavaPairRDD;
import org.apache.spark.api.java.JavaRDD;
import org.apache.spark.api.java.JavaSparkContext;
import scala.Tuple2;
import com.datastax.spark.connector.cql.CassandraConnector;
import static com.datastax.spark.connector.japi.CassandraJavaUtil.javaFunctions;
import static com.datastax.spark.connector.japi.CassandraJavaUtil.mapTupleToRow;
import static com.datastax.spark.connector.japi.CassandraJavaUtil.someColumns;

public class CassandraSparkWordCount extends AppBase {

  private static final Logger LOG = Logger.getLogger(CassandraSparkWordCount.class);
  // Static initialization of this workload's config.
  static {
    // Set the app type to simple.
    appConfig.appType = AppConfig.Type.Simple;

    // Set Spark parallelism for this app.
    appConfig.numWriterThreads = 2;
  }

  private boolean useCassandraInput;

  // input source (file or Cassandra table)
  private String inputFile;
  private String inputTableName;

  // output source (Cassandra table)
  private String outputTableName;

  // default values
  private static final String defaultInputTableName = "lines";
  private static final String defaultOutputTableName = "wordcounts";

  private CqlSession session;
  private JavaSparkContext sc = null;

  private void executeAndLog(CqlSession session, String stmt) {
    LOG.info(stmt);
    session.execute(stmt);
  }

  public CqlSession getCqlSession() {
    return session;
  }

  @Override
  public void initialize(CmdLineOpts configuration) {
    // Setup the local spark master, with the desired parallelism.
    SparkConf conf = new SparkConf().setAppName("yb.wordcount")
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

    //--------------------- Setting Input source (Cassandra table or file) -----------------------\\
    if (commandLine.hasOption("wordcount_input_file") &&
        commandLine.hasOption("wordcount_input_table")) {
      LOG.fatal("Input source can be EITHER file or table: found both options set");
      System.exit(1);
    } else if (commandLine.hasOption("wordcount_input_file")) {
      inputFile = commandLine.getOptionValue("wordcount_input_file");
      useCassandraInput = false;
      LOG.info("Using wordcount_input_file: " + inputFile);
    } else if (commandLine.hasOption("wordcount_input_table")) {
      inputTableName = commandLine.getOptionValue("wordcount_input_table");
      useCassandraInput = true;
      LOG.info("Using wordcount_input_table: " + inputTableName);
    } else { // defaults
      LOG.info("No input given, will create sample table and use it as input.");
      inputTableName = defaultInputTableName;
      useCassandraInput = true;

      // Setting up sample table

      // Drop the sample table if it already exists.
      dropCassandraTable(inputTableName);

      // Create the input table, and initialize the keyspace.
      AppBase.createKeyspace(session, getKeyspace());
      session.execute(SchemaBuilder
           .createTable(getKeyspace(), inputTableName)
           .ifNotExists()
           .withPartitionKey("id", DataTypes.INT)
           .withColumn("line", DataTypes.TEXT).build());

      // Insert some rows.
      String insert_stmt = "INSERT INTO " + getKeyspace() + "." + inputTableName
              + "(id, line) VALUES (%d, '%s');";
      executeAndLog(session, String.format(insert_stmt, 1,
              "ten nine eight seven six five four three two one"));
      executeAndLog(session, String.format(insert_stmt, 2,
              "ten nine eight seven six five four three two"));
      executeAndLog(session, String.format(insert_stmt, 3,
              "ten nine eight seven six five four three"));
      executeAndLog(session, String.format(insert_stmt, 4, "ten nine eight seven six five four"));
      executeAndLog(session, String.format(insert_stmt, 5, "ten nine eight seven six five"));
      executeAndLog(session, String.format(insert_stmt, 6, "ten nine eight seven six"));
      executeAndLog(session, String.format(insert_stmt, 7, "ten nine eight seven"));
      executeAndLog(session, String.format(insert_stmt, 8, "ten nine eight"));
      executeAndLog(session, String.format(insert_stmt, 9, "ten nine"));
      executeAndLog(session, String.format(insert_stmt, 10, "ten"));

      LOG.info("Created sample table " + inputTableName);
    }

    //----------------------- Setting Output location (Cassandra table) --------------------------\\
    if (commandLine.hasOption("wordcount_output_table")) {
      outputTableName = commandLine.getOptionValue("wordcount_input_table");
      LOG.info("Using wordcount_output_table: " + outputTableName);
    } else { // defaults
      outputTableName = defaultOutputTableName;
    }
  }

  @Override
  public void run() {
    //------------------------------------------- Input ------------------------------------------\\
    JavaRDD<String> rows;
    if (useCassandraInput) {
      // Read rows from table and convert them to an RDD.
      rows = javaFunctions(sc).cassandraTable(getKeyspace(), inputTableName)
              .select("line").map(row -> row.getString("line"));
    } else {
      // Read the input file and convert it to an RDD.
      rows = sc.textFile(inputFile);
    }

    // Perform the word count.
    JavaPairRDD<String, Integer> counts =
        rows.flatMap(line -> Arrays.asList(line.split(" ")).iterator())
            .mapToPair(word -> new Tuple2<String, Integer>(word, 1))
            .reduceByKey((x, y) ->  x + y);

    //------------------------------------------- Output -----------------------------------------\\

    String outTable = outputTableName;

    // Drop the output table if it already exists.
    dropCassandraTable(outTable);

    // Create the output table.
    session.execute(SchemaBuilder
           .createTable(getKeyspace(), outputTableName)
           .ifNotExists()
           .withPartitionKey("word", DataTypes.TEXT)
           .withColumn("count", DataTypes.INT).build());

    // Save the output to the CQL table.
    javaFunctions(counts).writerBuilder(getKeyspace(),
                                        outputTableName,
                                        mapTupleToRow(String.class, Integer.class))
        .withColumnSelector(someColumns("word", "count"))
        .saveToCassandra();

    sc.close();
  }

  public void close() {
    if (session != null) {
      session.close();
      session = null;
    }
  }

  @Override
  public List<String> getWorkloadDescription() {
    return Arrays.asList(
      "Simple Spark word count app that reads from an input table or file to compute ",
      "word count and saves results in an output table",
      "Input source is either input_file or input_table. If none is given a sample ",
      "Cassandra table " + defaultInputTableName + " is created and used as input");
  }

  @Override
  public List<String> getWorkloadOptionalArguments() {
    return Arrays.asList(
      "--num_threads_write " + appConfig.numWriterThreads,
      "--wordcount_output_table " + defaultOutputTableName,
      "--wordcount_input_file <path to input file>",
      "--wordcount_input_table <table name>");
  }
}
