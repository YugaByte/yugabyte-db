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

import com.yugabyte.sample.apps.AppBase.TableOp;

/**
 * This class encapsulates the various configuration parameters for the various apps.
 */
public class AppConfig {

  /**
   * The type of the current app. The valid types are:
   *   - OLTP   : These apps implement read() and write() methods. The read() and write() methods
   *              are called from independent threads. There is a metrics tracker that tracks
   *              latency and throughput.
   *   - Simple : These apps just implement a run() method. No metrics tracking is done.
   */
  public static enum Type {
    OLTP,
    Simple
  }
  // The app type, default initialized to OLTP.
  public Type appType = Type.OLTP;

  // The percentage of total threads that perform reads. The rest perform writes. Note that if this
  // value is 100, then no writes will happen. The plugin should have enough information as params
  // to be able to handle this scenario. This value is ignored if set to -1.
  public int readIOPSPercentage;

  // This is the exact number of reader threads.
  public int numReaderThreads;

  // This is the exact number of writer threads.
  public int numWriterThreads;

  // The number of keys to write as a part of this workload.
  public long numKeysToWrite;

  // The number of keys to read as a part of this workload. This ignores the attempts to read where
  // no data has yet to be written.
  public long numKeysToRead;

  // The size of the value to be written.
  public int valueSize;
  // The amount of time to wait before starting the next pipeline to the
  // server. sleepTime == 0 means that we'll wait for the pipeline to respond
  // before creating the next one i.e. we'll have only as many outstanding
  // pipelines as (numReaders + numWriters).
  public int sleepTime = 0;
  public int jedisSocketTimeout = 61000;

  public int cqlConnectTimeoutMs = 0;
  public int cqlReadTimeoutMs = 0;

  // Use ASCII strings as values.
  public boolean restrictValuesToAscii;

  // The number of unique keys to write, once these are written, the subsequent writes will be
  // updates to existing keys.
  public long numUniqueKeysToWrite;

  // Used for RedisHash workloads. The number of subkeys that we want per key.
  public int numSubkeysPerKey = 100;
  public int numSubkeysPerWrite = 10;
  public int numSubkeysPerRead = 10;
  public double keyUpdateFreqZipfExponent = 1.0;
  public double subkeyUpdateFreqZipfExponent = 1.0;
  public double valueSizeZipfExponent = 0;
  public int maxValueSize = 64 * 1024; // 64K

  // Maximum written key in case we reuse existing table.
  public long maxWrittenKey = -1;

  // The table level TTL in seconds. No TTL is applied if this value is set to -1.
  public long tableTTLSeconds = -1;

  // Batch size to send from client for redis pipeline app.
  public int redisPipelineLength = 1;

  // Batch size for apps that support batching.
  public int batchSize = 1;

  // Batch size to read for Cassandra batch timeseries app.
  public int cassandraReadBatchSize = 1;

  // Time interval to read for Cassandra batch timeseries app.
  public int readBackDeltaTimeFromNow = 100;

  // Perform sanity checks at the termination of app.
  public boolean sanityCheckAtEnd = false;

  // Disable YB load-balancing policy.
  public boolean disableYBLoadBalancingPolicy = false;

  // Read only workload without doing any writes before reads.
  public boolean readOnly = false;

  // UUID used by another app instance to write the keys.
  public String uuid;

  // Do not forward read requests to the leader.
  public boolean localReads = false;

  // Print all exceptions on the client instead of sampling.
  public boolean printAllExceptions = false;

  // Name of the table to create or drop. When set, used along with shouldDropTable to detect
  // the operation against the table name. If not provided on command line, defaults to the apps
  // default chosen table name.
  public String tableName = null;

  // Name of the default database for postgres.
  public String defaultPostgresDatabase = "postgres";

  // Name of the default username for postgres.
  public String defaultPostgresUsername = "postgres";

  // Should we drop / truncate table.
  public AppBase.TableOp tableOp = TableOp.NoOp;

  // Skip running workloads. These might be operations independent of workload.
  // For example, if we only need to drop a table, or any such DDL/non-CRUD op.
  public boolean skipWorkload = false;

  // The application name we are running.
  public String appName = "";

  // Run time for workload. Negative values means no limit.
  public long runTimeSeconds = -1;

  public String localDc;

  // Used by CassandraPersonalization workload.

  // Number of stores.
  public int numStores = 1;

  // Number of new coupons per customer in each load.
  public int numNewCouponsPerCustomer = 150;

  // Maximum number of coupons per customer.
  public int maxCouponsPerCustomer = 3000;

  // Use redis cluster client.
  public boolean useRedisCluster = false;

  // Create secondary index without transactions enabled.
  public boolean nonTransactionalIndex = false;

  // Enable batch write.
  public boolean batchWrite = false;

  // Username to connect to the DB.
  public String dbUsername = null;

  // Password to connect to the DB.
  public String dbPassword = null;

  // The number of client connections to establish to each host in the YugaByte DB cluster.
  public int concurrentClients = 4;

  // The path to the certificate to be used for the SSL connection.
  public String sslCert = null;
  // Number of devices to simulate data for CassandraEventData workload
  public int num_devices = 100;
  // Number of Event Types per device to simulate data for CassandraEventData workload
  public int num_event_types = 100;

  // Configurations for SqlDataLoad workload.
  public int numValueColumns = 1;
  public int numIndexes = 0;
  public int numForeignKeys = 0;
  public int numForeignKeyTableRows = 1000; // Only relevant if num_foreign_keys > 0.
  public int numConsecutiveRowsWithSameFk = 500; // Only relevant if num_foreign_keys > 0.

  // Configurations for SqlGeoPartitionedTable workload.
  public int numPartitions = 2;
}
