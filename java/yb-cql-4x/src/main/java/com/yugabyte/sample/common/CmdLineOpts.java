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

package com.yugabyte.sample.common;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Random;
import java.util.UUID;

import org.apache.commons.cli.BasicParser;
import org.apache.commons.cli.CommandLine;
import org.apache.commons.cli.CommandLineParser;
import org.apache.commons.cli.Option;
import org.apache.commons.cli.OptionBuilder;
import org.apache.commons.cli.Options;
import org.apache.commons.cli.ParseException;
import org.apache.log4j.Logger;

import com.google.common.collect.ImmutableList;

// Import * so we can list the sample apps.
import com.yugabyte.sample.apps.*;
import com.yugabyte.sample.apps.AppBase.TableOp;

/**
 * This is a helper class to parse the user specified command-line options if they were specified,
 * print help messages when running the app, etc.
 */
public class CmdLineOpts {
  private static final Logger LOG = Logger.getLogger(CmdLineOpts.class);

  // This is a unique UUID that is created by each instance of the application. This UUID is used in
  // various apps to make the keys unique. This allows us to run multiple instances of the app
  // safely.
  public static UUID loadTesterUUID;

  // The various apps present in this sample.
  private final static List<String> HELP_WORKLOADS = ImmutableList.of(
    "CassandraHelloWorld",
    "CassandraKeyValue",
    "CassandraRangeKeyValue",
    "CassandraBatchKeyValue",
    "CassandraBatchTimeseries",
    "CassandraEventData",
    "CassandraTransactionalKeyValue",
    "CassandraTransactionalRestartRead",
    "CassandraStockTicker",
    "CassandraTimeseries",
    "CassandraUserId",
    "CassandraPersonalization",
    "CassandraSecondaryIndex",
    "CassandraUniqueSecondaryIndex",
    "RedisKeyValue",
    "RedisPipelinedKeyValue",
    "RedisHashPipelined",
    "RedisYBClientKeyValue",
    "SqlDataLoad",
    "SqlForeignKeysAndJoins",
    "SqlInserts",
    "SqlSecondaryIndex",
    "SqlSnapshotTxns",
    "SqlUpdates"
  );

  // The class type of the app needed to spawn new objects.
  private Class<? extends AppBase> appClass;
  // List of database contact points. One contact point could be resolved to multiple nodes IPs.
  public List<ContactPoint> contactPoints = new ArrayList<>();
  // The number of reader threads to spawn for OLTP apps.
  int numReaderThreads;
  // The number of writer threads to spawn for OLTP apps.
  int numWriterThreads;
  boolean readOnly = false;
  boolean localReads = false;
  // Random number generator.
  Random random = new Random();
  // Command line opts parser.
  CommandLine commandLine;

  public void initialize(CommandLine commandLine) throws ClassNotFoundException {
    this.commandLine = commandLine;
    if (commandLine.hasOption("uuid")) {
      loadTesterUUID = UUID.fromString(commandLine.getOptionValue("uuid"));
      LOG.info("Using given UUID : " + loadTesterUUID);
    } else if (commandLine.hasOption("nouuid")) {
      loadTesterUUID = null;
      LOG.info("Using NO UUID");
    } else {
      loadTesterUUID = UUID.randomUUID();
      LOG.info("Using a randomly generated UUID : " + loadTesterUUID);
    }

    // Get the workload.
    String appName = commandLine.getOptionValue("workload");
    appClass = getAppClass(appName);
    LOG.info("App: " + appClass.getSimpleName());

    AppBase.appConfig.appName = appClass.getSimpleName();

    if (commandLine.hasOption("skip_workload")) {
      AppBase.appConfig.skipWorkload = true;
      // For now, drop table is the only op that is permitted without workload.
      if (!commandLine.hasOption("drop_table_name")) {
        LOG.error("Table name to be dropped is expected when skipping workload run.");
        System.exit(1);
      }
    }

    if (commandLine.hasOption("run_time")) {
      AppBase.appConfig.runTimeSeconds =
          Integer.parseInt(commandLine.getOptionValue("run_time"));
    }
    LOG.info("Run time (seconds): " + AppBase.appConfig.runTimeSeconds);

    // Get the proxy contact points.
    List<String> hostPortList = Arrays.asList(commandLine.getOptionValue("nodes").split(","));
    for (String hostPort : hostPortList) {
      LOG.info("Adding node: " + hostPort);
      this.contactPoints.add(ContactPoint.fromHostPort(hostPort));
    }

    // This check needs to be done before initializeThreadCount is called.
    if (commandLine.hasOption("read_only")) {
      AppBase.appConfig.readOnly = true;
      readOnly = true;
      if (!commandLine.hasOption("uuid") && !commandLine.hasOption("nouuid")) {
        LOG.error("uuid (or nouuid) needs to be provided when using --read-only");
        System.exit(1);
      }
    }

    // Set the number of threads.
    initializeThreadCount(commandLine);
    // Initialize num keys.
    initializeNumKeys(commandLine);
    // Initialize table properties.
    initializeTableProperties(commandLine);
    if (commandLine.hasOption("local_reads")) {
      AppBase.appConfig.localReads = true;
      localReads = true;
    }
    LOG.info("Local reads: " + localReads);
    LOG.info("Read only load: " + readOnly);

    if (commandLine.hasOption("batch_size")) {
      AppBase.appConfig.batchSize =
              Integer.parseInt(commandLine.getOptionValue("batch_size"));

        if (AppBase.appConfig.batchSize > AppBase.appConfig.numUniqueKeysToWrite) {
          LOG.fatal("The batch size cannot be more than the number of unique keys");
          System.exit(-1);
        }
        LOG.info("Batch size : " + AppBase.appConfig.batchSize);
    }

    if (commandLine.hasOption("with_local_dc")) {
      if (AppBase.appConfig.disableYBLoadBalancingPolicy == true) {
        LOG.error("--disable_yb_load_balancing_policy cannot be used with --with_local_dc");
        System.exit(1);
      }
      AppBase.appConfig.localDc = commandLine.getOptionValue("with_local_dc");
    }
    if (commandLine.hasOption("use_redis_cluster")) {
      AppBase.appConfig.useRedisCluster = true;
    }
    if (commandLine.hasOption("username")) {
      AppBase.appConfig.dbUsername = commandLine.getOptionValue("username");
    }
    if (commandLine.hasOption("password")) {
      if (!commandLine.hasOption("username")) {
        LOG.error("--password requires --username to be set");
        System.exit(1);
      }
      AppBase.appConfig.dbPassword = commandLine.getOptionValue("password");
    }
    if (commandLine.hasOption("concurrent_clients")) {
      AppBase.appConfig.concurrentClients = Integer.parseInt(
          commandLine.getOptionValue("concurrent_clients"));
    }
    if (commandLine.hasOption("ssl_cert")) {
      AppBase.appConfig.sslCert = commandLine.getOptionValue("ssl_cert");
    }

    if (commandLine.hasOption("num_indexes")) {
      AppBase.appConfig.numIndexes =
          Integer.parseInt(commandLine.getOptionValue("num_indexes"));
    }
  }

  /**
   * Creates new instance of the app.
   * @return the app instance.
   */
  public AppBase createAppInstance() {
    return createAppInstance(true /* enableMetrics */);
  }

  /**
   * Creates new instance of the app.
   * @param enableMetrics Should metrics tracker be enabled.
   * @return the app instance.
   */
  public AppBase createAppInstance(boolean enableMetrics) {
    AppBase workload = null;
    try {
      // Create a new workload object.
      workload = appClass.newInstance();
      // Initialize the workload.
      workload.workloadInit(this, enableMetrics);
    } catch (Exception e) {
      LOG.error("Could not create instance of " + appClass.getName(), e);
    }
    return workload;
  }

  public CommandLine getCommandLine() {
    return commandLine;
  }

  public List<ContactPoint> getContactPoints() {
    return contactPoints;
  }

  public ContactPoint getRandomContactPoint() {
    int contactPointId = random.nextInt(contactPoints.size());
    LOG.debug("Returning random contact point id " + contactPointId);
    return contactPoints.get(contactPointId);
  }

  public int getNumReaderThreads() {
    return numReaderThreads;
  }

  public int getNumWriterThreads() {
    return numWriterThreads;
  }

  public boolean getReadOnly() {
    return readOnly;
  }

  public boolean doErrorChecking() {
    return AppBase.appConfig.sanityCheckAtEnd;
  }

  public boolean shouldDropTable() {
    return AppBase.appConfig.tableOp == TableOp.DropTable;
  }

  public boolean skipWorkload() {
    return AppBase.appConfig.skipWorkload;
  }

  public String appName() {
    return AppBase.appConfig.appName;
  }

  private static Class<? extends AppBase> getAppClass(String workloadType)
      throws ClassNotFoundException {
    // Get the workload class.
    return Class.forName("com.yugabyte.sample.apps." + workloadType)
                         .asSubclass(AppBase.class);
  }

  private void initializeThreadCount(CommandLine cmd) {
    // Check if there are a fixed number of threads or variable.
    String numThreadsStr = cmd.getOptionValue("num_threads");
    if (readOnly) {
      numReaderThreads = AppBase.appConfig.numReaderThreads;
      numWriterThreads = 0;
    } else if (AppBase.appConfig.readIOPSPercentage == -1) {
      numReaderThreads = AppBase.appConfig.numReaderThreads;
      numWriterThreads = AppBase.appConfig.numWriterThreads;
    } else {
      int numThreads = 0;
      if (numThreadsStr != null) {
          numThreads = Integer.parseInt(numThreadsStr);
      } else {
        // Default to 8 * num-cores
        numThreads = 8 * Runtime.getRuntime().availableProcessors();
      }
      numReaderThreads =
          (int) Math.round(1.0 * numThreads * AppBase.appConfig.readIOPSPercentage / 100);
      numWriterThreads = numThreads - numReaderThreads;
    }

    // If number of read and write threads are specified on the command line, that overrides all
    // the other values.
    if (cmd.hasOption("num_threads_read")) {
      numReaderThreads = Integer.parseInt(cmd.getOptionValue("num_threads_read"));
    }
    if (cmd.hasOption("num_threads_write")) {
      if (readOnly) {
        LOG.warn("Ignoring num_threads_write option. It shouldn't be used with read_only.");
      } else {
        numWriterThreads = Integer.parseInt(cmd.getOptionValue("num_threads_write"));
      }
    }
    LOG.info("Num reader threads: " + numReaderThreads +
             ", num writer threads: " + numWriterThreads);
  }

  private void initializeNumKeys(CommandLine cmd) {
    if (cmd.hasOption("num_writes")) {
      AppBase.appConfig.numKeysToWrite = Long.parseLong(cmd.getOptionValue("num_writes"));
    }
    if (cmd.hasOption("num_reads")) {
      AppBase.appConfig.numKeysToRead = Long.parseLong(cmd.getOptionValue("num_reads"));
    }
    if (cmd.hasOption("num_unique_keys")) {
      AppBase.appConfig.numUniqueKeysToWrite =
          Long.parseLong(cmd.getOptionValue("num_unique_keys"));
    }
    AppBase.appConfig.maxWrittenKey = Long.parseLong(cmd.getOptionValue("max_written_key",
        String.valueOf(AppBase.appConfig.maxWrittenKey)));
    if (cmd.hasOption("value_size")) {
      AppBase.appConfig.valueSize = Integer.parseInt(cmd.getOptionValue("value_size"));
    }
    if (cmd.hasOption("sleep_time")) {
      AppBase.appConfig.sleepTime =
          Integer.parseInt(cmd.getOptionValue("sleep_time"));
    }
    if (cmd.hasOption("socket_timeout")) {
      AppBase.appConfig.jedisSocketTimeout =
          Integer.parseInt(cmd.getOptionValue("socket_timeout"));
    }
    if (cmd.hasOption("cql_connect_timeout_ms")) {
      AppBase.appConfig.cqlConnectTimeoutMs =
          Integer.parseInt(cmd.getOptionValue("cql_connect_timeout_ms"));
    }
    if (cmd.hasOption("cql_read_timeout_ms")) {
      AppBase.appConfig.cqlReadTimeoutMs =
          Integer.parseInt(cmd.getOptionValue("cql_read_timeout_ms"));
    }
    if (cmd.hasOption("use_ascii_values")) {
      AppBase.appConfig.restrictValuesToAscii = true;
    }
    if (cmd.hasOption("sanity_check_at_end")) {
      AppBase.appConfig.sanityCheckAtEnd = true;
    }
    if (cmd.hasOption("disable_yb_load_balancing_policy")) {
      AppBase.appConfig.disableYBLoadBalancingPolicy = true;
    }
    if (cmd.hasOption("print_all_exceptions")) {
      AppBase.appConfig.printAllExceptions = true;
    }
    if (cmd.hasOption("create_table_name") && cmd.hasOption("drop_table_name")) {
      LOG.error("Both create and drop table options cannot be provided together.");
      System.exit(1);
    }
    if (cmd.hasOption("keep_table")) {
      if (cmd.hasOption("drop_table_name")) {
        LOG.error("Both keep and drop table options cannot be provided together.");
        System.exit(1);
      }
      AppBase.appConfig.tableOp = TableOp.NoOp;
    }
    if (cmd.hasOption("create_table_name")) {
      AppBase.appConfig.tableName = cmd.getOptionValue("create_table_name");
      LOG.info("Create table name: " + AppBase.appConfig.tableName);
    }
    if (cmd.hasOption("truncate")) {
      AppBase.appConfig.tableOp = TableOp.TruncateTable;
      LOG.info("Going to truncate table");
    }
    if (cmd.hasOption("default_postgres_database")) {
      AppBase.appConfig.defaultPostgresDatabase = cmd.getOptionValue("default_postgres_database");
      LOG.info("Default postgres database: " + AppBase.appConfig.defaultPostgresDatabase);
    }
    if (cmd.hasOption("drop_table_name")) {
      AppBase.appConfig.tableName = cmd.getOptionValue("drop_table_name");
      LOG.info("Drop table name: " + AppBase.appConfig.tableName);
      AppBase.appConfig.tableOp = TableOp.DropTable;
    }
    LOG.info("Num unique keys to insert: " + AppBase.appConfig.numUniqueKeysToWrite);
    LOG.info("Num keys to update: " +
        (AppBase.appConfig.numKeysToWrite - AppBase.appConfig.numUniqueKeysToWrite));
    LOG.info("Num keys to read: " + AppBase.appConfig.numKeysToRead);
    LOG.info("Value size: " + AppBase.appConfig.valueSize);
    LOG.info("Restrict values to ASCII strings: " + AppBase.appConfig.restrictValuesToAscii);
    LOG.info("Perform sanity check at end of app run: " + AppBase.appConfig.sanityCheckAtEnd);
  }

  private void initializeTableProperties(CommandLine cmd) {
    // Initialize the TTL.
    if (cmd.hasOption("table_ttl_seconds")) {
      AppBase.appConfig.tableTTLSeconds =
          Long.parseLong(cmd.getOptionValue("table_ttl_seconds"));
    }

    LOG.info("Table TTL (secs): " + AppBase.appConfig.tableTTLSeconds);
  }

  /**
   * Creates a command line opts object from the arguments specified on the command line.
   * @param args command line args.
   * @return a CmdLineOpts object.
   * @throws java.lang.Exception exceptions during parsing and preparing of options.
   */
  public static CmdLineOpts createFromArgs(String[] args) throws Exception {
    Options options = new Options();

    Option appType = OptionBuilder.create("workload");
    appType.setDescription("The workload to run.");
    appType.setRequired(true);
    appType.setLongOpt("workload");
    appType.setArgs(1);
    options.addOption(appType);

    Option proxyAddrs = OptionBuilder.create("nodes");
    proxyAddrs.setDescription("Comma separated proxies, host1:port1,....,hostN:portN");
    proxyAddrs.setRequired(true);
    proxyAddrs.setLongOpt("nodes");
    proxyAddrs.setArgs(1);
    options.addOption(proxyAddrs);

    options.addOption("help", false, "Show help message.");
    options.addOption("verbose", false, "Enable debug level logging.");

    options.addOption("uuid", true, "The UUID to use for this loadtester.");
    options.addOption("nouuid", false,
                      "Do not use a UUID. Keys will be key:1, key:2, key:3, "
                          + "instead of <uuid>:1, <uuid>:2, <uuid>:3 etc.");
    options.addOption("create_table_name", true, "The name of the CQL table to create.");
    options.addOption("default_postgres_database", true, "The name of the default postgres db.");
    options.addOption("drop_table_name", true, "The name of the CQL table to drop.");
    options.addOption("truncate", false, "Whether to truncate the table at the beginning.");
    options.addOption("read_only", false, "Read-only workload. " +
        "Values must have been written previously and uuid must be provided. " +
        "num_threads_write will be ignored.");
    options.addOption("keep_table", false, "Keep the table at the beginning of application launch");
    options.addOption("local_reads", false, "Use consistency ONE for reads.");
    options.addOption("num_threads", true, "The total number of threads.");
    options.addOption("num_threads_read", true, "The number of threads that perform reads.");
    options.addOption("num_threads_write", true, "The number of threads that perform writes.");
    options.addOption("num_writes", true, "The total number of writes to perform.");
    options.addOption("num_reads", true, "The total number of reads to perform.");
    options.addOption(
        "sleep_time", true,
        "How long (in ms) to sleep between multiple pipeline batches.");
    options.addOption("socket_timeout", true,
                      "How long (in ms) to wait for a response from jedis.");
    options.addOption("cql_connect_timeout_ms", true, "Connection timeout for cql in millisecs");
    options.addOption("cql_read_timeout_ms", true, "Read timeout for cql in millisecs");
    options.addOption("value_size", true, "Size in bytes of the value. " +
        "The bytes are random. Value size should be more than 5 (9) bytes for binary (ascii) " +
        "values in order to have checksum for read verification. First byte is used as a " +
        "binary/ascii marker. If value size is more than 16 bytes, random content is prepended " +
        "with \"val: $key\" string, so we can check if value matches the key during read.");
    options.addOption("use_ascii_values", false, "[RedisKeyValue] If " +
        "specified, values are restricted to ASCII strings.");
    options.addOption("table_ttl_seconds", true, "The TTL in seconds to create the table with.");
    options.addOption("sanity_check_at_end", false,
        "Add FATAL logs to ensure no failures before terminating the app.");
    options.addOption("disable_yb_load_balancing_policy", false,
        "Disable Yugabyte load-balancing policy.");
    options.addOption("print_all_exceptions", false,
        "Print all exceptions encountered on the client, instead of sampling.");
    options.addOption("skip_workload", false, "Skip running workload.");
    options.addOption("run_time", true,
        "Run time for workload. Negative value means forever (default).");
    options.addOption("use_redis_cluster", false, "Use redis cluster client.");
    options.addOption("username", true,
        "User name to connect to the database using. ");
    options.addOption("password", true,
        "The password to use when connecting to the database. " +
            "If this option is set, the --username option is required.");
    options.addOption("concurrent_clients", true,
        "The number of client connections to establish to each host in the YugaByte DB cluster.");
    options.addOption("ssl_cert", true,
      "Use an SSL connection while connecting to YugaByte.");
    options.addOption("batch_size", true,
                      "Number of keys to write in a batch (for apps that support batching).");

    // Options for CassandraTimeseries workload.
    options.addOption("num_users", true, "[CassandraTimeseries] The total number of users.");
    options.addOption("min_nodes_per_user", true,
                      "[CassandraTimeseries] The min number of nodes per user.");
    options.addOption("min_nodes_per_user", true,
                      "[CassandraTimeseries] The min number of nodes per user.");
    options.addOption("max_nodes_per_user", true,
                      "[CassandraTimeseries] The max number of nodes per user.");
    options.addOption("min_metrics_count", true,
                      "[CassandraTimeseries] The min number of metrics for all nodes of user.");
    options.addOption("max_metrics_count", true,
                      "[CassandraTimeseries] The max number of metrics for all nodes of user.");
    options.addOption("data_emit_rate_millis", true,
                      "[CassandraTimeseries] The rate at which each node emits all metrics.");

    // Options for CassandraStockTicker workload.
    options.addOption("num_ticker_symbols", true,
                      "[CassandraStockTicker] The total number of stock ticker symbols.");

    // Options for the key-value workloads.
    options.addOption("num_unique_keys", true,
                      "[KV workloads only] Number of unique keys to write into the DB.");
    options.addOption("max_written_key", true,
        "[KV workloads only, reusing existing table] Max written key number.");

    // Options for CassandraBatchTimeseries app.
    options.addOption("read_batch_size", true,
                      "[CassandraBatchTimeseries] Number of keys to read in a batch.");
    options.addOption("read_back_delta_from_now", true,
                      "[CassandraBatchTimeseries] Time unit delta back from current time unit.");

    options.addOption("with_local_dc", true, "Local DC name.");
    // Options for CassandraEventData app.
    options.addOption("read_batch_size", true,
      "[CassandraEventData] Number of keys to read in a batch.");
    options.addOption("num_devices", true,
      "[CassandraEventData] Number of devices to generate data");
    options.addOption("num_event_types", true,
      "[CassandraEventData] Number of event types to generate per device");
    options.addOption("read_batch_size", true,
      "[CassandraEventData] Number of keys to read in a batch.");
    options.addOption("read_back_delta_from_now", true,
      "[CassandraEventData] Time unit delta back from current time unit.");

    // Options for CassandraPersonalization app.
    options.addOption("num_stores", true,
                      "[CassandraPersonalization] Number of stores.");
    options.addOption("num_new_coupons_per_customer", true,
                      "[CassandraPersonalization] Number of new coupons per customer.");
    options.addOption("max_coupons_per_customer", true,
                      "[CassandraPersonalization] Maximum number of coupons per customer.");

    // Options for CassandraSecondaryIndex app.
    options.addOption("non_transactional_index", false,
                      "[CassandraSecondaryIndex] Create secondary index without transactions " +
                      "enabled.");
    options.addOption("batch_write", false,
                      "[CassandraSecondaryIndex] Enable batch write of key values.");

    // Options for Redis Pipelined Key Value
    options.addOption(
        "pipeline_length", true,
        "[RedisPipelinedKeyValue/RedisHashPipelined] Number of commands to be sent out"
            + " in a redis pipelined sync.");

    options.addOption(
        "num_subkeys_per_key", true,
        "[RedisHashPipelined] Number of subkeys in each key for the RedisHashPipelined workload.");
    options.addOption(
        "num_subkeys_per_write", true,
        "[RedisHashPipelined] Number of subkeys updated in each HMSet "
            + "for the RedisHashPipelined workload.");
    options.addOption(
        "num_subkeys_per_read", true,
        "[RedisHashPipelined] Number of subkeys read in each HMGet "
            + "for the RedisHashPipelined workload.");

    options.addOption(
        "key_freq_zipf_exponent", true,
        "[RedisHashPipelined] The zipf distribution exponent, if keys " +
        "should be picked using a Zipf distribution. If <= 0, we use " +
        "a uniform distribution");
    options.addOption(
        "subkey_freq_zipf_exponent", true,
        "[RedisHashPipelined] The zipf distribution exponent, if subkeys " +
        "should be picked using a Zipf distribution. If <= 0, we use " +
        "a uniform distribution");
    options.addOption(
        "subkey_value_size_zipf_exponent", true,
        "[RedisHashPipelined] The zipf distribution exponent, if the value " +
        "sizes should be picked using a Zipf distribution. Value sizes are " +
        "chosen such that the expected mean is the value specified by --value_size. " +
        "If <= 0, all subkeys will have the value specified by --value_size");
    options.addOption(
        "subkey_value_max_size", true,
        "[RedisHashPipelined] If using zipf distribution to choose value sizes, " +
        "specifies an upper bound on the value sizes.");

    // Options for SQL DataLoad.
    options.addOption("num_value_columns", true,
                      "[SqlDataLoad] Number of value columns the target table should have.");

    options.addOption("num_indexes", true,
                      "[SqlDataLoad] Number of secondary indexes on the target table.");

    options.addOption("num_foreign_keys", true,
                      "[SqlDataLoad] Number of secondary indexes on the target table.");

    options.addOption("num_foreign_key_table_rows", true,
                      "[SqlDataLoad] Number of secondary indexes on the target table.");

    options.addOption("num_consecutive_rows_with_same_fk", true,
                      "[SqlDataLoad] Number of secondary indexes on the target table.");

    options.addOption("num_partitions", true,
                      "[SqlGeoPartitionedTable] Number of partitions to create.");

    // First check if a "--help" argument is passed with a simple parser. Note that if we add
    // required args, then the help string would not work.
    // The first function check if help was called with an app name to print details. The second
    // function just check if help was called without any args to print the overview.
    parseHelpDetailed(args, options);
    parseHelpOverview(args, options);

    // Do the actual arg parsing.
    CommandLineParser parser = new BasicParser();
    CommandLine commandLine = null;

    try {
      commandLine = parser.parse(options, args);
    } catch (ParseException e) {
      LOG.error("Error in args, use the --help option to see usage. Exception:", e);
      System.exit(0);
    }

    CmdLineOpts configuration = new CmdLineOpts();
    configuration.initialize(commandLine);
    return configuration;
  }

  private static void parseHelpOverview(String[] args, Options options) throws Exception {
    Options helpOptions = new Options();
    helpOptions.addOption("help", false, "Print help.");
    CommandLineParser helpParser = new BasicParser();
    CommandLine helpCommandLine = null;
    try {
      helpCommandLine = helpParser.parse(helpOptions, args);
    } catch (ParseException e) {
      return;
    }
    if (helpCommandLine.hasOption("help")) {
      printUsage(options, "Usage:");
      System.exit(0);
    }
  }

  private static void parseHelpDetailed(String[] args, Options options) throws Exception {
    Options helpOptions = new Options();
    helpOptions.addOption("help", true, "Print help.");
    CommandLineParser helpParser = new BasicParser();
    CommandLine helpCommandLine = null;
    try {
      helpCommandLine = helpParser.parse(helpOptions, args);
    } catch (org.apache.commons.cli.MissingArgumentException e1) {
      // This is ok since there was no help argument passed.
      return;
    }  catch (ParseException e) {
      return;
    }
    if (helpCommandLine.hasOption("help")) {
      printUsageDetails(options, "Usage:", helpCommandLine.getOptionValue("help"));
      System.exit(0);
    }
  }

  private static int getAppPort(String appName) {
    if (appName.startsWith("Cassandra")) return 9042;
    else if (appName.startsWith("Redis")) return 6379;
    else if (appName.startsWith("Sql")) return 5433;
    return 0;
  }

  private static void printUsage(Options options, String header) throws Exception {
    StringBuilder footer = new StringBuilder();

    footer.append("****************************************************************************\n");
    footer.append("*                                                                          *\n");
    footer.append("*                     YugaByte DB Sample Apps                              *\n");
    footer.append("*                                                                          *\n");
    footer.append("****************************************************************************\n");
    footer.append("\n");
    footer.append("Use this sample app to try out a variety of workloads against YugaByte DB.\n");
    footer.append("  Use the --help <app name> option to get more details on how to run it.\n");
    String optsPrefix = "\t\t\t";
    String optsSuffix = " \\\n";
    for (String workloadType: HELP_WORKLOADS) {
      AppBase workload = getAppClass(workloadType).newInstance();
      String formattedName = String.format("%-35s: ", workloadType);
      footer.append("\n  * " + formattedName);
      List<String> description = workload.getWorkloadDescription();
      if (!description.isEmpty()) {
        footer.append(description.get(0));
      }
    }
    footer.append("\n");
    System.out.println(footer.toString());
    System.exit(0);
  }

  private static void printUsageDetails(Options options, String header, String appName)
    throws Exception {
    StringBuilder footer = new StringBuilder();

    footer.append("Usage and options for workload " + appName + " in YugaByte DB Sample Apps.\n");
    String optsPrefix = "\t\t\t";
    String optsSuffix = " \\\n";
    int port = getAppPort(appName);
    AppBase workload = getAppClass(appName).newInstance();

    footer.append("\n - " + appName + " :\n");
    footer.append("   ");
    for (int idx = 0; idx < appName.length(); idx++) {
      footer.append("-");
    }
    footer.append("\n");

    List<String> description = workload.getWorkloadDescription();
    if (!description.isEmpty()) {
      for (String line : description) {
        footer.append("\t\t");
        footer.append(line);
        footer.append("\n");
      }
      footer.append("\n");
    }
    footer.append("\t\tUsage:\n");
    footer.append(optsPrefix);
    footer.append("java -jar yb-sample-apps.jar");
    footer.append(optsSuffix);
    footer.append(optsPrefix + "--workload " + appName + optsSuffix);
    footer.append(optsPrefix + "--nodes 127.0.0.1:" + port);

    List<String> requiredArgs = workload.getWorkloadRequiredArguments();
    for (String line : requiredArgs) {
      footer.append(optsSuffix).append(optsPrefix).append(line);
    }

    List<String> optionalArgs = workload.getWorkloadOptionalArguments();
    if (!optionalArgs.isEmpty()) {
      footer.append("\n\n\t\tOther options (with default values):\n");
      for (String line : optionalArgs) {
        footer.append(optsPrefix + "[ ");
        footer.append(line);
        footer.append(" ]\n");
      }
    }
    footer.append("\n");
    System.out.println(footer.toString());
    System.exit(0);
  }

  /**
   * One contact point could be resolved to multiple nodes IPs. For example for Kubernetes
   * yb-tservers.default.svc.cluster.local contact point is resolved to all tservers IPs.
   */
  public static class ContactPoint {
    String host;
    int port;

    public ContactPoint(String host, int port) {
      this.host = host;
      this.port = port;
    }

    public String getHost() {
      return host;
    }

    public int getPort() {
      return port;
    }

    public static ContactPoint fromHostPort(String hostPort) {
      int portSplit = hostPort.lastIndexOf(":");
      if (portSplit < 0) {
        throw new RuntimeException("Invalid port specification in " + hostPort);
      }
      String host = hostPort.substring(0, portSplit);
      int port = Integer.parseInt(hostPort.substring(portSplit + 1));
      return new ContactPoint(host, port);
    }

    public String ToString() { return host + ":" + port; }
  }
}
