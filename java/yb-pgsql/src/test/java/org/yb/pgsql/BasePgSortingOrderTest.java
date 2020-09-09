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

package org.yb.pgsql;

import static org.yb.AssertionWrappers.*;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.List;
import java.util.Arrays;
import java.util.Set;
import java.util.HashSet;

public abstract class BasePgSortingOrderTest extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(BasePgSortingOrderTest.class);

  private static final Set<String> supportedTypes = new HashSet<>(Arrays.asList(
    "BIGINT",
    "BIGSERIAL",
    "BIT",
    "BIT (10)",
    "BIT VARYING (10)",
    "VARBIT",
    "VARBIT (10)",
    "BOOLEAN",
    "BOX",
    "BYTEA",
    "CHAR (10)",
    "CHARACTER (10)",
    "VARCHAR (10)",
    "CHARACTER VARYING (10)",
    "CIDR",
    "CIRCLE",
    "DATE",
    "DEC",
    "DECIMAL",
    "DOUBLE PRECISION",
    "FLOAT",
    "FLOAT (3)",
    "FLOAT2",
    "FLOAT4",
    "FLOAT8",
    "INET",
    "INT",
    "INT2",
    "INT4",
    "INT8",
    "INTEGER",
    "INTERVAL",
    "JSON",
    "JSONB",
    "LINE",
    "LSEG",
    "MACADDR",
    "MACADDR8",
    "MONEY",
    "NUMERIC",
    "NUMERIC (10, 3)",
    "PATH",
    "PG_LSN",
    "POINT",
    "POLYGON",
    "REAL",
    "SERIAL",
    "SERIAL2",
    "SERIAL4",
    "SERIAL8",
    "SMALLINT",
    "SMALLSERIAL",
    "TEXT",
    "TIME",
    "TIME (3) WITHOUT TIME ZONE",
    "TIME (3) WITH TIME ZONE",
    "TIME WITHOUT TIME ZONE",
    "TIME WITH TIME ZONE",
    "TIMESTAMP",
    "TIMESTAMP (3) WITHOUT TIME ZONE",
    "TIMESTAMP (3) WITH TIME ZONE",
    "TIMESTAMP WITHOUT TIME ZONE",
    "TIMESTAMP WITH TIME ZONE",
    "TIMESTAMPTZ",
    "TIMETZ",
    "TSQUERY",
    "TSVECTOR",
    "TXID_SNAPSHOT",
    "UUID",
    "XML"
  ));

  public String formTableName(String typeName) {
    String name = typeName.replaceAll("[ ()]", "_");
    return name;
  }

  public void createTables(String[] typeNames) throws SQLException {
    LOG.info("CREATE TABLES - Start");
    for (String typeName : typeNames) {
      if (!supportedTypes.contains(typeName)) {
        throw new RuntimeException("Unknown type name: " + typeName);
      }

      try (Statement statement = connection.createStatement()) {
        String sql = String.format("CREATE TABLE tab_%s" +
                                   "(id bool, datum %s, PRIMARY KEY(id, datum));",
                                   formTableName(typeName), typeName);
        statement.execute(sql);
        LOG.info("CREATED: " + sql);
      }
    }
    LOG.info("CREATE TABLES - Done");
  }

  public void createTablesWithInvalidPrimaryKey(String... invalidTypeNames) throws SQLException {
    LOG.info("CREATE TABLES WITH INVALID PRIMARY KEY - Start");
    for (String typeName : invalidTypeNames) {
      if (!supportedTypes.contains(typeName)) {
        throw new RuntimeException("Unknown type name: " + typeName);
      }

      try (Statement statement = connection.createStatement()) {
        String sql = String.format("CREATE TABLE tab_%s" +
                                   "(id bool, datum %s, PRIMARY KEY(id, datum));",
                                   formTableName(typeName), typeName);
        runInvalidQuery(statement, sql, "not yet supported");
      }
    }
    LOG.info("CREATE TABLES WITH INVALID PRIMARY KEY - Done");
  }

  public void insertValues(String tableName, String[] values) throws Exception {
    LOG.info("INSERT VALUES - Start");

    // Constructing the SQL statement.
    // INSERT INTO tab_xxx VALUES (true, datum_value);
    // - Hash value should always true to keep all data into one tablet.
    // - DocDB should insert and read "datum_value" in ASC order.
    String stmt = "INSERT INTO tab_" + tableName + " VALUES ";
    boolean first = true;
    for (String value : values) {
      if (first) {
        first = false;
      } else {
        stmt += ", ";
      }
      stmt += "(true, " + value + ")";
    }
    stmt += ";";

    // Execute the statement.
    try (Statement statement = connection.createStatement()) {
      LOG.info("EXECUTE: " + stmt);
      statement.execute(stmt);
    }

    LOG.info("INSERT VALUES - Done");
  }

  public void insertInvalidValues(String tableName, String[] values) throws Exception {
    LOG.info("INSERT INVALID VALUES - Start");

    // Constructing the SQL statement.
    String stmtFormat = "INSERT INTO tab_" + tableName + " VALUES (true, %s);";
    for (String value : values) {
      // Execute the statement.
      String stmt = String.format(stmtFormat, value);
      try (Statement statement = connection.createStatement()) {
        // Specific error message depends on a value, we're not verifying it here.
        runInvalidQuery(statement, stmt, "ERROR");
      }
    }

    LOG.info("INSERT INVALID VALUES - Done");
  }

  public void selectAndCompare(String tableName, int rowCount) throws Exception {
    String pgsqlStmt = "SELECT datum FROM tab_" + tableName + " ORDER BY datum;";
    String docdbStmt = "SELECT datum FROM tab_" + tableName + ";";

    // Make sure that DocDB order is exactly the same as Postgres ORDER BY.
    try (Statement statement = connection.createStatement()) {
      List<Row> pgsqlRows;
      List<Row> docdbRows;
      try (ResultSet rs = statement.executeQuery(pgsqlStmt)) {
        pgsqlRows = getRowList(rs);
      }
      try (ResultSet rs = statement.executeQuery(docdbStmt)) {
        docdbRows = getRowList(rs);
      }

      LOG.info("Comparing result for " + tableName +
               "\n  SQL Order = " + pgsqlRows.toString() +
               "\n  DOC Order = " + docdbRows.toString());
      assertEquals(pgsqlRows.size(), rowCount);
      assertEquals(docdbRows.size(), rowCount);
      assertEquals(pgsqlRows, docdbRows);
    }
  }

  // Run one testcase.
  public void runSortingOrderTest(String[] typeNames,
                                  String[][] values,
                                  String[][] invalidValues) throws Exception {
    // Create all table to test if a type is supported.
    createTables(typeNames);

    // Check the sorting order of each datatype.
    int count = typeNames.length;
    for (int i = 0; i < count; i++) {
      String typeName = typeNames[i];
      String tableName = formTableName(typeName);
      LOG.info("Testing sorting order for " + typeName);

      // Insert values.
      insertValues(tableName, values[i]);
      insertInvalidValues(tableName, invalidValues[i]);

      // Check the ordering in DocDB.
      selectAndCompare(tableName, values[i].length);
    }
  }

  @Override
  public int getTestMethodTimeoutSec() {
    return 1800;
  }
}
