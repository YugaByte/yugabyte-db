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

import org.junit.After;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.client.TestUtils;
import org.yb.pgsql.PgRegressRunner;
import org.yb.util.YBTestRunnerNonTsanOnly;
import org.yb.util.StringUtil;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.IOException;
import java.lang.ProcessBuilder;
import java.sql.*;
import java.util.*;
import java.util.regex.Pattern;

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestYsqlDump extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestYsqlDump.class);

  private static final int TURN_OFF_SEQUENCE_CACHE_FLAG = 0;

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("ysql_sequence_cache_minval", Integer.toString(TURN_OFF_SEQUENCE_CACHE_FLAG));
    return flagMap;
  }

  // The following logic is needed to remove the dependency on the exact version number from
  // the ysql_dump output part that looks like this:
  // -- Dumped from database version 11.2-YB-1.3.2.0-b0
  // -- Dumped by ysql_dump version 11.2-YB-1.3.2.0-b0

  private static String VERSION_STR_PREFIX = " version ";
  private static Pattern VERSION_NUMBER_PATTERN = Pattern.compile(
      VERSION_STR_PREFIX + "[0-9]+[.][0-9]+-YB-([0-9]+[.]){3}[0-9]+-b[0-9]+");
  private static String VERSION_NUMBER_REPLACEMENT_STR =
      VERSION_STR_PREFIX + "X.X-YB-X.X.X.X-bX";

  private String postprocessOutputLine(String s) {
    if (s == null)
      return null;
    return StringUtil.rtrim(
      VERSION_NUMBER_PATTERN.matcher(s).replaceAll(VERSION_NUMBER_REPLACEMENT_STR));
  }

  private static void expectOnlyEmptyLines(String curLine, BufferedReader in) throws IOException {
    while (curLine != null) {
      assertEquals("", curLine.trim());
      curLine = in.readLine();
    }
  }

  @Test
  public void testPgDump() throws Exception {
    // Location of Postgres regression tests
    File pgRegressDir = PgRegressRunner.getPgRegressDir();

    // Create the data
    try (BufferedReader inputIn = createFileReader(new File(pgRegressDir,
                                                            "sql/yb_ysql_dump.sql"))) {
      try (Statement statement = connection.createStatement()) {
        String inputLine = null;
        while ((inputLine = inputIn.readLine()) != null) {
          LOG.info(inputLine);
          statement.execute(inputLine);
          LOG.info("Executed");
        }
      }
    }

    // Dump and validate the data
    File pgBinDir = PgRegressRunner.getPgBinDir();
    File ysqlDumpExec = new File(pgBinDir, "ysql_dump");

    final int tserverIndex = 0;
    File actual = new File(pgRegressDir, "output/yb_ysql_dump.out");
    ProcessBuilder pb = new ProcessBuilder(ysqlDumpExec.toString(), "-h", getPgHost(tserverIndex),
                                           "-p", Integer.toString(getPgPort(tserverIndex)),
                                           "-U", DEFAULT_PG_USER,
                                           "-f", actual.toString());
    pb.start().waitFor();
    try (BufferedReader actualIn   = createFileReader(actual);
         BufferedReader expectedIn = createFileReader(new File(pgRegressDir,
                                                              "expected/yb_ysql_dump.out"));) {
      String actualLine = null, expectedLine = null;

      while ((actualLine = actualIn.readLine()) != null &&
             (expectedLine = expectedIn.readLine()) != null) {
        assertEquals(postprocessOutputLine(expectedLine), postprocessOutputLine(actualLine));
      }
      expectOnlyEmptyLines(actualLine, actualIn);
      expectOnlyEmptyLines(expectedLine, expectedIn);
    }
  }

  private BufferedReader createFileReader(File f) throws Exception {
    return new BufferedReader(new FileReader(f));
  }
}
