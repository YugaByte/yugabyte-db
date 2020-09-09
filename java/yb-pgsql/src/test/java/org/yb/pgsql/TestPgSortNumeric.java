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
public class TestPgSortNumeric extends BasePgSortingOrderTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgSortNumeric.class);

  // All typename MUST be in upper case for comparison purpose.
  static String testTypes[] = {
    "BIGINT",      // INT8
    "INT",         // INT4, INTEGER
    "SMALLINT",    // INT2
    "FLOAT4",      // REAL, FLOAT(1-24)
    "FLOAT8",      // DOUBLE PRECISION, FLOAT, FLOAT(25-53)
    "DECIMAL",     // DEC, NUMERIC
    // Serial types:
    "BIGSERIAL",   // SERIAL8
    "SERIAL",      // SERIAL4
    "SMALLSERIAL", // SERIAL2

    "MONEY",
    "PG_LSN",      // Internally LSN (Log Sequence Number) is a 64-bit integer
  };

  static String[][] testValues = {
    // BIGINT
    { "9223372036854775807", "-9223372036854775808", "0", "1", "-1", "100", "-100" },

    // INT
    { "2147483647", "-2147483648", "0", "1", "-1", "100", "-100"},

    // SMALLINT
    { "32767", "-32768", "0", "1", "-1", "100", "-100" },

    // FLOAT4
    { "1.4012984643E-45", "-1.4012984643E-45", "1.1754942107E-38", "-1.1754942107E-38",
      "1.1754943508E-38", "-1.1754943508E-38", "3.4028234664E+38", "-3.4028234664E+38",
      "0.9999999404", "-0.9999999404", "1.0000001192", "-1.0000001192",
      "0", "12.34", "-12.34", "'NaN'", "'Infinity'", "'-Infinity'" },

    // FLOAT8
    { "4.9406564584124654E-324", "-4.9406564584124654E-324",
      "2.2250738585072009E-308", "-2.2250738585072009E-308",
      "2.2250738585072014E-308", "-2.2250738585072014E-308",
      "1.7976931348623157E+308", "-1.7976931348623157E+308",
      "1.0000000000000002", "-1.0000000000000002", "0.9999999999999998", "-0.9999999999999998",
      "0", "56.78", "-56.78", "'NaN'", "'Infinity'", "'-Infinity'" },

    // DECIMAL
    { "12345678901234567890123456789012345678901234567890123456789012345678901234567890123456",
      "-12345678901234567890123456789012345678901234567890123456789012345678901234567890123456",
      "0.12345678901234567890123456789012345678901234567890123456789012345678901234567890123456",
      "-0.12345678901234567890123456789012345678901234567890123456789012345678901234567890123456",
      "1.2E-16382", "-1.2E-16382", "1.2E+131071", "-1.2E+131071", "0", "12.34", "-12.34" },

    // BIGSERIAL
    { "9223372036854775807", "-9223372036854775808", "0", "1", "-1", "100", "-100" },

    // SERIAL
    { "2147483647", "-2147483648", "0", "1", "-1", "100", "-100"},

    // SMALLSERIAL
    { "32767", "-32768", "0", "1", "-1", "100", "-100" },

    // MONEY
    { "'0'", "'1'", "'-1'", "'100'", "'-100'", "'2.0001'", "'5,.06'", "'5,.6'", "'$3.0001'",
      "'$40'", "'1,2'", "'1,23'",
      // TOFIX: https://github.com/YugaByte/yugabyte-db/issues/1949
      // "'-92233720368547758.08'", "'92233720368547758.07'",
      // "'100,120'", "'100,23'", "'1000,23'", "'1,000,000.12'", "'2,000.00012'",
      // "'$3,000.00012'", "'$4,000,000.12'"
    },

    // PG_LSN
    { "'DEADBEAF/DEADBEAF'", "'7FFFFFFF/FFFFFFFF'", "'80000000/00000000'",
      "'0/0'", "'0/1'", "'FFFFFFFF/FFFFFFFF'", "'0/64'", "'FFFFFFFF/FFFFFF9C'" },
  };

  static String[][] testInvalidValues = {
    // BIGINT
    { "9223372036854775808", "-9223372036854775809", "NULL" },

    // INT
    { "2147483648", "-2147483649", "NULL" },

    // SMALLINT
    { "32768", "-32769", "NULL" },

    // FLOAT4
    { "NULL", "'nan'", "NaN", "'Inf'", "'-Inf'", "Infinity", "-Infinity",
      "1.4E-46", "-1.4E-46", "3.5E+38", "-3.5E+38" },

    // FLOAT8
    { "NULL", "'nan'", "NaN", "'Inf'", "'-Inf'", "Infinity", "-Infinity",
      "4.9E-325", "-4.9E-325", "1.8E+308", "-1.8E+308" },

    // DECIMAL
    { "NULL", "'NaN'", "'Infinity'", "'-Infinity'", "NaN", "Infinity", "-Infinity",
      "Inf", "-Inf", "1.2E-16383", "-1.2E-16383", "1.2E+131072", "-1.2E+131072" },

    // BIGSERIAL
    { "NULL", "9223372036854775808", "-9223372036854775809" },

    // SERIAL
    { "NULL", "2147483648", "-2147483649" },

    // SMALLSERIAL
    { "NULL", "32768", "-32769" },

    // MONEY
    { "NULL", "1", "A", "\\xFF", "'1.000,12'", "'@1,000'" },

    // PG_LSN
    { "NULL", "0", "1", "-1", "A" },
  };

  // Testing sorting order for the listed types.
  @Test
  public void testSort() throws Exception {
    runSortingOrderTest(testTypes, testValues, testInvalidValues);
  }
}
