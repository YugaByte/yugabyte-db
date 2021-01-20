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
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Statement;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgDdl extends BasePgSQLTest {

  @Test
  public void testLargeTransaction() throws Exception {
    try (Statement stmt = connection.createStatement()) {
      stmt.execute("BEGIN");
      for (int i = 0; i < 111; ++i) {
        // Table is created with a single tablet for performance reason.
        stmt.execute(String.format("CREATE TABLE table_%d (k INT) SPLIT INTO 1 TABLETS", i + 1));
      }
      stmt.execute("COMMIT");
    }
  }
}
