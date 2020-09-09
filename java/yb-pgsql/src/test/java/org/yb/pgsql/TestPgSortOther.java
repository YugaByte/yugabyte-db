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
public class TestPgSortOther extends BasePgSortingOrderTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSortOther.class);

  // All typename MUST be in upper case for comparison purpose.
  static String testTypes[] = {
    "BYTEA",
    "UUID",
  };

  static String[][] testValues = {
    // BYTEA
    { "'\\x01'", "'\\xFF'", "'\\x00'", "'\\x01020AFF'", "'\\x00000000'", "'\\xFFFFFFFF'",
      "'\\xDEADBEAF'", "'\\\\x30'", "'0'", "'X'", "'0xQQQ'", "'hello'", "'\t10'", "'    20'",
      "'   XYZ   '" },

    // UUID
    { "'{aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa}'", "'ffffffff-ffff-ffff-ffff-ffffffffffff'",
      "'12345678-1234-1234-1234-123456789ABC'", "'DEADBEAF-DEAD-BEAF-DEAD-BEAFDEADBEAF'",
      "'00000000-0000-0000-0000-000000000000'", "'AbCdEf00-AbCd-AbCd-AbCd-AbCdEfAbCdEf'" }
  };

  static String[][] testInvalidValues = {
    // BYTEA
    { "NULL", "0", "A", "0x00", "\t00", "\\\t00", "\\\\x00", "'\\\t00'", "\\03", "' \\x02'" },

    // UUID
    { "NULL", "0", "  0", "A", "0x00", "\t00", "'0'", "'  0'", "'A'", "'0x00'", "'\t00'",
      "12345678-1234-1234-1234-123456789012" },
  };

  // Testing sorting order for the listed types.
  @Test
  public void testSort() throws Exception {
    runSortingOrderTest(testTypes, testValues, testInvalidValues);

    // Geometric types.
    createTablesWithInvalidPrimaryKey("BOX", "CIRCLE", "LINE", "LSEG", "PATH", "POINT", "POLYGON");
    // Network types.
    createTablesWithInvalidPrimaryKey("CIDR", "INET", "MACADDR", "MACADDR8");
    // Test other invalid type names.
    createTablesWithInvalidPrimaryKey("JSON", "JSONB", "TXID_SNAPSHOT", "XML");
  }
}
