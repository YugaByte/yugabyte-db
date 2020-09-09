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

import static org.yb.AssertionWrappers.*;

@RunWith(value=YBTestRunnerNonTsanOnly.class)
public class TestPgSortChar extends BasePgSortingOrderTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSortNumeric.class);

  // All typename MUST be in upper case for comparison purpose.
  static String testTypes[] = {
    "CHARACTER (10)",
    "CHARACTER VARYING (10)",
    "TEXT"
  };

  static String[][] testValues = {
    // CHAR(10)
    { "'abc'", "'xyz'", "'lmn'" },

    // VARCHAR(10)
    { "'abc'", "'xyz'", "'lmn'" },

    // TEXT
    { "'abc'", "'xyz'", "'lmn'" },
  };

  static String[][] testInvalidValues = {
    // CHAR(10)
    { "'12345678901'", "NULL" },

    // VARCHAR(10)
    { "'12345678901'", "NULL" },

    // TEXT
    { "NULL" }
  };

  // Testing sorting order for the listed types.
  @Test
  public void testSort() throws Exception {
    runSortingOrderTest(testTypes, testValues, testInvalidValues);

    // Test invalid type names.
    createTablesWithInvalidPrimaryKey("BIT", "VARBIT", "TSQUERY", "TSVECTOR");
  }
}
