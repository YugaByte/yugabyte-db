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

import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.List;
import java.util.Map;

import org.postgresql.util.PSQLException;
import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgExplicitLocks extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSelect.class);
  private static final int kSlowdownPgsqlAggregateReadMs = 2000;

  @Test
  public void testExplicitLocks() throws Exception {
    Statement statement = connection.createStatement();
    List<Row> allRows = setupSimpleTable("explicitlocks");

    Connection c1 = getConnectionBuilder().connect();
    Connection c2 = getConnectionBuilder().connect();

    Statement s1 = c1.createStatement();
    Statement s2 = c2.createStatement();

    try {
      String query = "begin";
      s1.execute(query);
      query = "select * from explicitlocks where h=0 and r=0 for update";
      s1.execute(query);
    } catch (PSQLException ex) {
      LOG.error("Unexpected exception:", ex);
      throw ex;
    }

    boolean conflict_occured = false;
    try {
      String query = "update explicitlocks set vi=5 where h=0 and r=0";
      s2.execute(query);
    } catch (PSQLException ex) {
      if (ex.getMessage().contains("Conflicts with higher priority transaction")) {
        LOG.info("Conflict ERROR");
        conflict_occured = true;
      }
    }
    assertEquals(conflict_occured, true);
    LOG.info("Done with the test");
  }
}
