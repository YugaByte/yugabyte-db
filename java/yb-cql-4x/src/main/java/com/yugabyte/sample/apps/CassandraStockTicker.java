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
import java.util.Date;
import java.util.List;
import java.util.Random;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicLong;

import org.apache.commons.cli.CommandLine;
import org.apache.log4j.Logger;

import com.datastax.oss.driver.api.core.cql.BoundStatement;
import com.datastax.oss.driver.api.core.cql.PreparedStatement;
import com.datastax.oss.driver.api.core.cql.ResultSet;
import com.datastax.oss.driver.api.core.cql.Row;
import com.yugabyte.sample.common.CmdLineOpts;
import com.yugabyte.sample.common.TimeseriesLoadGenerator;

public class CassandraStockTicker extends AppBase {
  private static final Logger LOG = Logger.getLogger(CassandraStockTicker.class);
  // Static initialization of this workload's config.
  static {
    // Disable the read-write percentage.
    appConfig.readIOPSPercentage = -1;
    // Set the read and write threads to 1 each.
    appConfig.numReaderThreads = 32;
    appConfig.numWriterThreads = 4;
    // Set the number of keys to read and write.
    appConfig.numKeysToRead = -1;
    appConfig.numKeysToWrite = -1;
    // Set the TTL for the raw table.
    appConfig.tableTTLSeconds = 24 * 60 * 60;
  }
  private static int num_ticker_symbols = 10000;
  // The rate at which each metric is generated in millis.
  private static long data_emit_rate_millis = 1 * 1000;
  // The structure to hold all the stock ticker info.
  static List<TickerInfo> tickers = new CopyOnWriteArrayList<TickerInfo>();
  // The table that has the raw ticker data.
  public static String tickerTableRaw = "stock_ticker_raw";
  // The table that has the downsampled minutely data.
  private static String tickerTableMin = "stock_ticker_1min";
  // The total number of rows read from the DB so far.
  private static AtomicLong num_rows_read;
  static Random random = new Random();
  // The shared prepared select statement for fetching the latest data point.
  private static volatile PreparedStatement preparedSelectLatest;
  // The shared prepared statement for inserting into the raw table.
  private static volatile PreparedStatement preparedInsertRaw;
  // The shared prepared statement for inserting into the 1 min table.
  private static volatile PreparedStatement preparedInsertMin;
  // Lock for initializing prepared statement objects.
  private static final Object prepareInitLock = new Object();

  @Override
  public void initialize(CmdLineOpts configuration) {
    synchronized (tickers) {
      LOG.info("Initializing CassandraStockTicker app...");
      // If the datasources have already been created, we have already initialized the static
      // variables, so nothing to do.
      if (!tickers.isEmpty()) {
        return;
      }

      // Read the various params from the command line.
      CommandLine commandLine = configuration.getCommandLine();
      if (commandLine.hasOption("num_ticker_symbols")) {
        num_ticker_symbols = Integer.parseInt(commandLine.getOptionValue("num_ticker_symbols"));
        LOG.info("num_ticker_symbols: " + num_ticker_symbols);
      }
      if (commandLine.hasOption("data_emit_rate_millis")) {
        data_emit_rate_millis =
            Long.parseLong(commandLine.getOptionValue("data_emit_rate_millis"));
        LOG.info("data_emit_rate_millis: " + data_emit_rate_millis);
      }

      int ticker_symbol_idx = 0;
      while (ticker_symbol_idx < num_ticker_symbols) {
        // Create the data source.
        TickerInfo dataSource = new TickerInfo(ticker_symbol_idx, data_emit_rate_millis);
        tickers.add(dataSource);
        ticker_symbol_idx++;
        if (ticker_symbol_idx % 1000 == 0) {
          System.out.print(".");
        }
      }
      System.out.println("");
      // Initialize the number of rows read.
      num_rows_read = new AtomicLong(0);
      LOG.info("CassandraStockTicker app is ready.");
    }
  }

  @Override
  public void dropTable() {
    // Drop the raw table.
    dropCassandraTable(tickerTableRaw);
    // Drop the minutely table.
    dropCassandraTable(tickerTableMin);
  }

  @Override
  public List<String> getCreateTableStatements() {
    // Create the raw table.
    String create_stmt_raw = "CREATE TABLE IF NOT EXISTS " + tickerTableRaw + " (" +
                             "  ticker_id varchar" +
                             ", ts timestamp" +
                             ", value varchar" +
                             ", primary key ((ticker_id), ts))" +
                             " WITH CLUSTERING ORDER BY (ts DESC)";
    if (appConfig.tableTTLSeconds > 0) {
      create_stmt_raw += " AND default_time_to_live = " + appConfig.tableTTLSeconds;
    }
    create_stmt_raw += ";";

    // Create the minutely table.
    String create_stmt_min = "CREATE TABLE IF NOT EXISTS " + tickerTableMin + " (" +
                             "  ticker_id varchar" +
                             ", ts timestamp" +
                             ", value varchar" +
                             ", primary key ((ticker_id), ts))" +
                             " WITH CLUSTERING ORDER BY (ts DESC)";
    if (appConfig.tableTTLSeconds > 0) {
    create_stmt_min += " AND default_time_to_live = " + (60 * appConfig.tableTTLSeconds);
    }
    create_stmt_min += ";";
    return Arrays.asList(create_stmt_raw, create_stmt_min);
  }

  private PreparedStatement getPreparedSelectLatest()  {
    if (preparedSelectLatest == null) {
      synchronized (prepareInitLock) {
        if (preparedSelectLatest == null) {
          // Create the prepared statement object.
          String select_stmt =
              String.format("SELECT * from %s WHERE ticker_id = ? LIMIT 1", tickerTableRaw);
          preparedSelectLatest = getCassandraClient().prepare(select_stmt);
        }
      }
    }
    return preparedSelectLatest;
  }

  @Override
  public long doRead() {
    // Pick a ransom data source.
    TickerInfo dataSource = tickers.get(random.nextInt(tickers.size()));
    // Make sure it has emitted data, otherwise there is nothing to read.
    if (!dataSource.getHasEmittedData()) {
      return 0;
    }
    long startTs = dataSource.getStartTs();
    long endTs = dataSource.getEndTs();

    // Bind the select statement.
    BoundStatement select = getPreparedSelectLatest().bind(dataSource.getTickerId());
    // Make the query.
    ResultSet rs = getCassandraClient().execute(select);

    List<Row> rows = rs.all();
    num_rows_read.addAndGet(rows.size());
    return 1;
  }

  private PreparedStatement getPreparedInsertRaw()  {
    if (preparedInsertRaw == null) {
      synchronized (prepareInitLock) {
        if (preparedInsertRaw == null) {
          // Create the prepared statement object.
          String insert_stmt =
              String.format("INSERT INTO %s (ticker_id, ts, value) VALUES (?, ?, ?)",
                            tickerTableRaw);
          preparedInsertRaw = getCassandraClient().prepare(insert_stmt);
        }
      }
    }
    return preparedInsertRaw;
  }

  private PreparedStatement getPreparedInsertMin()  {
    if (preparedInsertMin == null) {
      synchronized (prepareInitLock) {
        if (preparedInsertMin == null) {
          // Create the prepared statement object.
          String insert_stmt =
              String.format("INSERT INTO %s (ticker_id, ts, value) VALUES (?, ?, ?)",
                            tickerTableMin);
          preparedInsertMin = getCassandraClient().prepare(insert_stmt);
        }
      }
    }
    return preparedInsertMin;
  }

  @Override
  public synchronized void resetClients() {
    synchronized (prepareInitLock) {
      preparedInsertMin = null;
      preparedInsertRaw = null;
      preparedSelectLatest = null;
    }
    super.resetClients();
  }

  @Override
  public synchronized void destroyClients() {
    synchronized (prepareInitLock) {
      preparedInsertMin = null;
      preparedInsertRaw = null;
      preparedSelectLatest = null;
    }
    super.destroyClients();
  }

  @Override
  public long doWrite(int threadIdx) {
    // Pick a random data source.
    TickerInfo dataSource = tickers.get(random.nextInt(tickers.size()));
    // Enter as many data points as are needed.
    long ts = dataSource.getDataEmitTs();
    long numKeysWritten = 0;
    String value = String.format("value-%s", ts);
    // If we have nothing to write, we're done.
    if (ts == -1) {
      try {
        Thread.sleep(100 /* millisecs */);
      } catch (Exception e) {}
      return 0; /* numKeysWritten */
    }

    // Insert the row.
    BoundStatement insertRaw =
        getPreparedInsertRaw().bind(dataSource.getTickerId(), new Date(ts), value);
    ResultSet resultSet = getCassandraClient().execute(insertRaw);
    numKeysWritten++;
    dataSource.setLastEmittedTs(ts);

    // With some probability, insert into the minutely table.
    if (random.nextInt(60000) < data_emit_rate_millis) {
      BoundStatement insertMin =
          getPreparedInsertMin().bind(dataSource.getTickerId(), new Date(ts), value);
      resultSet = getCassandraClient().execute(insertMin);
      numKeysWritten++;
    }

    return numKeysWritten;
  }

  @Override
  public void appendMessage(StringBuilder sb) {
    super.appendMessage(sb);
    sb.append("Rows read: " + num_rows_read.get());
    sb.append(" | ");
  }

  /**
   * This class represents a single data source, which sends back timeseries data for a bunch of
   * metrics. Each data source generates data for all metrics at the same time interval, which is
   * governed by emit rate.
   */
  public static class TickerInfo extends TimeseriesLoadGenerator {
    // The ticker id this data source represents.
    String ticker_id;
    // The data emit rate.
    long dataEmitRateMs;

    public TickerInfo(int ticker_idx, long dataEmitRateMs) {
      super(ticker_idx, dataEmitRateMs, appConfig.tableTTLSeconds * 1000L);
      this.dataEmitRateMs = dataEmitRateMs;
      this.ticker_id = super.getId();
    }

    public String getTickerId() {
      return ticker_id;
    }

    public long getEndTs() {
      return getLastEmittedTs() + 1;
    }

    public long getStartTs() {
      // Return an interval that reads 30-120 data points.
      long deltaT = 30L * (1 + random.nextInt(90)) * dataEmitRateMs;
      return getEndTs() - deltaT;
    }

    @Override
    public String toString() {
      return getId();
    }
  }

  @Override
  public List<String> getWorkloadDescription() {
    return Arrays.asList(
      "Sample stock ticker app built on CQL. Models stock tickers each of which emits quote data",
      " every second. The raw data is written into the 'stock_ticker_raw' table, which retains",
      " data for one day. The 'stock_ticker_1min' table models downsampled ticker data, is",
      " written to once a minute and retains data for 60 days. Every read query gets the latest",
      " value of the stock symbol from the 'stock_ticker_raw' table.");
  }

  @Override
  public List<String> getWorkloadOptionalArguments() {
    return Arrays.asList(
      "--num_threads_read " + appConfig.numReaderThreads,
      "--num_threads_write " + appConfig.numWriterThreads,
      "--num_ticker_symbols " + num_ticker_symbols,
      "--data_emit_rate_millis " + data_emit_rate_millis,
      "--table_ttl_seconds " + appConfig.tableTTLSeconds);
  }
}
