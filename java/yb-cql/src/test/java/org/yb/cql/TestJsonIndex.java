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
package org.yb.cql;

import com.datastax.driver.core.Cluster;
import com.datastax.driver.core.KeyspaceMetadata;
import com.datastax.driver.core.TableMetadata;
import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import org.junit.Test;
import org.json.*;

import java.util.List;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertTrue;

import org.yb.client.TestUtils;
import org.yb.YBTestRunner;

import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestJsonIndex extends BaseCQLTest {

  @Test
  public void testIndex() throws Exception {
    session.execute("CREATE TABLE test_json_index" +
                    "  ( h INT PRIMARY KEY, a_column INT, j1 JSONB, j2 JSONB )" +
                    "  with transactions = {'enabled' : true};");
    session.execute("CREATE INDEX jidx ON test_json_index(j1->'a'->>'b')");
    session.execute("CREATE INDEX cidx ON test_json_index(a_column)");

    int h;
    for (h = 0; h < 3000; h++) {
      String jvalue = String.format("{ \"a\" : { \"b\" : \"bvalue_%d\" }," +
                                    "  \"a_column\" : %d }", h, h );
      String stmt = String.format("INSERT INTO test_json_index(h, j1, j2) VALUES (%d, '%s', '%s');",
                                  h, jvalue, jvalue);
      session.execute(stmt);
    }

    // Insert various value formats to the JSONB column to make sure that the JSONB expression
    // index supports null values.
    session.execute(String.format("INSERT INTO test_json_index(h) values (%d);", h++));
    session.execute(String.format("INSERT INTO test_json_index(h, j1) values (%d, 'null');", h++));
    session.execute(String.format("INSERT INTO test_json_index(h, j1) values (%d, '\"abc\"');",
                                  h++));
    session.execute(String.format("INSERT INTO test_json_index(h, j1) values (%d, '3');", h++));
    session.execute(String.format("INSERT INTO test_json_index(h, j1) values (%d, 'true');", h++));
    session.execute(String.format("INSERT INTO test_json_index(h, j1) values (%d, 'false');", h++));
    session.execute(String.format("INSERT INTO test_json_index(h, j1) values (%d, '2.0');", h++));

    // Run index scan and check the time.
    String query = "SELECT h FROM test_json_index WHERE j1->'a'->>'b' = 'bvalue_77';";
    // Not timing the first run.
    assertEquals(1, session.execute(query).all().size());

    long runtimeMillis = System.currentTimeMillis();
    assertEquals(1, session.execute(query).all().size());
    long elapsedTimeMillis_index = System.currentTimeMillis() - runtimeMillis;
    LOG.info(String.format("Indexed query: Elapsed time = %d msecs", elapsedTimeMillis_index));

    // Scenarios 1: Full scan and check the time - Column "j2" is not indexed.
    query = "SELECT h FROM test_json_index WHERE j2->'a'->>'b' = 'bvalue_88';";
    // Not timing the first run.
    assertEquals(1, session.execute(query).all().size());

    runtimeMillis = System.currentTimeMillis();
    assertEquals(1, session.execute(query).all().size());
    long elapsedTimeMillis_full = System.currentTimeMillis() - runtimeMillis;
    LOG.info(String.format("Full scan query: Elapsed time = %d msecs", elapsedTimeMillis_full));

    // Do performance testing ONLY in RELEASE build.
    if (TestUtils.isReleaseBuild()) {
      // Check that full-scan is 5 times slower than index-scan.
      assertTrue((elapsedTimeMillis_full/3.0) >= elapsedTimeMillis_index);
    }

    // Scenarios 2: Full scan and check the time. Attribute "j1->>a_column" is not indexed
    // even though column "a_column" is indexed.
    query = "SELECT h FROM test_json_index WHERE j1->>'a_column' = '99';";
    // Not timing the first run.
    assertEquals(1, session.execute(query).all().size());

    runtimeMillis = System.currentTimeMillis();
    assertEquals(1, session.execute(query).all().size());
    elapsedTimeMillis_full = System.currentTimeMillis() - runtimeMillis;
    LOG.info(String.format("Full scan query: Elapsed time = %d msecs", elapsedTimeMillis_full));

    // Do performance testing ONLY in RELEASE build.
    if (TestUtils.isReleaseBuild()) {
      // Check that full-scan is slower than index-scan.
      assertTrue((elapsedTimeMillis_full/3.0) >= elapsedTimeMillis_index);
    }
  }

  @Test
  public void testOrderByJson() throws Exception {
    session.execute("CREATE TABLE test_order (a text," +
                    "                         b text," +
                    "                         j jsonb," +
                    "                         PRIMARY KEY (a, b))" +
                    "  WITH CLUSTERING ORDER BY (b ASC) AND default_time_to_live = 0;");

    session.execute("CREATE INDEX test_order_index ON test_order (b, j->'a'->>'b')" +
                    "  INCLUDE (a)" +
                    "  WITH CLUSTERING ORDER BY (j->'a'->>'b' DESC)" +
                    "    AND transactions = { 'enabled' : FALSE, " +
                    "                         'consistency_level' : 'user_enforced' };");

    // rowDesc is the query result in descending order, rowAsc, ascending.
    String rowDesc = "";
    String rowAsc = "";

    int rowCount = 10;
    int jMax = 99;
    int jDesc;
    int jAsc = jMax - rowCount;
    String jValue;
    String a;
    String b = "index_hash";

    // INSERT rows to be selected with order by.
    for (int i = 0; i < rowCount; i++) {
      jDesc = jMax - i;
      a = String.format("a_%d", jDesc);
      jValue = String.format("{\"a\":{\"b\":\"j_%d\"}}", jDesc);

      rowDesc += String.format("Row[%s, %s, %s]", a, b, jValue);
      session.execute(String.format("INSERT INTO test_order (a, b, j) VALUES('%s', '%s', '%s');",
                                    a, b, jValue));

      jAsc++;
      a = String.format("a_%d", jAsc);
      jValue = String.format("{\"a\":{\"b\":\"j_%d\"}}", jAsc);
      rowAsc += String.format("Row[%s, %s, %s]", a, b, jValue);
    }

    // INSERT dummy rows that should be filtered out by the query.
    b = "dummy";
    jDesc = 99;
    for (int i = 0; i < rowCount; i++) {
      jDesc = jMax - i;
      a = String.format("a_%d", jDesc);
      jValue = String.format("{\"a\":{\"b\":\"j_%d\"}}", jDesc);
      session.execute(String.format("INSERT INTO test_order (a, b, j) VALUES('%s', '%s', '%s');",
                                    a, b, jValue));
    }

    // Asserting query result.
    assertQuery("SELECT * FROM test_order WHERE b = 'index_hash';", rowDesc);
    assertQuery("SELECT * FROM test_order WHERE b = 'index_hash' " +
                "  ORDER BY j->'a'->>'b' DESC;", rowDesc);
    assertQuery("SELECT * FROM test_order WHERE b = 'index_hash' " +
                "  ORDER BY j->'a'->>'b' ASC;", rowAsc);
  }

  @Test
  public void testIndexSyntax() throws Exception {
    session.execute("CREATE TABLE test_json_index_syntax" +
                    "  ( \"j->'a'->>'b'\" INT PRIMARY KEY, \"j->'a'->>'c'\" INT, j JSONB ) " +
                    "  WITH TRANSACTIONS = {'enabled' : true};");

    // Valid indexes: No name conflict.
    session.execute("CREATE INDEX jidx1 ON test_json_index_syntax(j->'a'->>'d');");
    session.execute("CREATE INDEX jidx2 ON test_json_index_syntax(j->'a'->>'c');");
    session.execute("CREATE INDEX jidx3 ON test_json_index_syntax" +
                    "  (j->'a'->>'d', \"j->'a'->>'c'\");");
    session.execute("CREATE INDEX jidx4 ON test_json_index_syntax(j->'a'->>'d')" +
                    "  INCLUDE (\"j->'a'->>'c'\")");

    // Invalid indexes: Name conflict.
    runInvalidQuery("CREATE INDEX jidx5 ON test_json_index_syntax(j->'a'->>'b')");
    runInvalidQuery("CREATE INDEX jidx6 ON test_json_index_syntax(j->'a'->>'c')" +
                    "  INCLUDE (\"j->'a'->>'c'\")");
    runInvalidQuery("CREATE INDEX jidx7 ON test_json_index_syntax" +
                    "  (j->'a'->>'c', \"j->'a'->>'c'\");");
    runInvalidQuery("CREATE INDEX jidx8 ON test_json_index_syntax" +
                    "  (\"j->'a'->>'c'\", j->'a'->>'c');");
  }

  @Test
  public void testIndexEscapeName() throws Exception {
    // Check name mangling operations.
    String schema_stmt = "CREATE SCHEMA TestJsonIndex;";
    String table_stmt = "CREATE TABLE TestJsonIndex.test_json_index_escape" +
                        "  ( \"C$_C$$C_col\" INT PRIMARY KEY," +
                        "    \"$_$$_col\" INT," +
                        "    \"C$_col->>'$J_attr'\" JSONB ) " +
                        "  WITH TRANSACTIONS = {'enabled' : true};";
    String index_stmt =
        "CREATE INDEX jidx9 ON TestJsonIndex.test_json_index_escape" +
        "  (\"C$_col->>'$J_attr'\"->>'\"J$_attr->>C$_col\"')" +
        "  INCLUDE (\"$_$$_col\");";
    // JAVA driver does not show INCLUDE, so the expected string should not contain it.
    String expected_index_desc =
        "CREATE INDEX jidx9 ON TestJsonIndex.test_json_index_escape" +
        " (C$_col->>'$J_attr'->>'\"J$_attr->>C$_col\"', c$_c$$c_col);";

    // Create INDEX to test mangling column names.
    session.execute(schema_stmt);
    session.execute(table_stmt);
    session.execute(index_stmt);

    // Describe INDEX to test demangling column names.
    KeyspaceMetadata ks_metadata = cluster.getMetadata().getKeyspace("TestJsonIndex");
    TableMetadata tab_metadata = ks_metadata.getTable("test_json_index_escape");
    String tab_desc = tab_metadata.exportAsString();
    assertTrue(tab_desc.toLowerCase().contains(expected_index_desc.toLowerCase()));

    // INSERT and SELECT to test using mangled names.
    int rowCount = 10;
    String rowDesc = "";
    for (int i = 0; i < rowCount; i++) {
      String insert_stmt =
        String.format("INSERT INTO TestJsonIndex.test_json_index_escape" +
                      "  (\"C$_C$$C_col\", \"$_$$_col\", \"C$_col->>'$J_attr'\")" +
                      "  VALUES(%d, %d, '{ \"\\\"J$_attr->>C$_col\\\"\" : \"json_%d\" }');",
                      i, 1000 + i, 10000 + i);
      session.execute(insert_stmt);
    }

    for (int i = 0; i < rowCount; i++) {
      String insert_stmt =
        String.format("INSERT INTO TestJsonIndex.test_json_index_escape" +
                      "  (\"C$_C$$C_col\", \"$_$$_col\", \"C$_col->>'$J_attr'\")" +
                      "  VALUES(%d, %d, '{ \"\\\"J$_attr->>C$_col\\\"\" : \"%s\" }');",
                      i, 1000 + i, "json_hash_value");
      session.execute(insert_stmt);
      rowDesc += String.format("Row[%d]", 1000 + i);
    }

    // Asserting query result.
    assertQuery("SELECT \"$_$$_col\" FROM TestJsonIndex.test_json_index_escape" +
                "  WHERE \"C$_col->>'$J_attr'\"->>'\"J$_attr->>C$_col\"' = 'json_hash_value';",
                rowDesc);
  }

  @Test
  public void testIndexSimilarColumnName() throws Exception {
    session.execute("CREATE TABLE test_similar_column_name" +
                    "  ( h INT PRIMARY KEY, v INT, vv INT, j JSONB )" +
                    "  with transactions = {'enabled' : true};");

    // Test case for covering-check using ID resolution.
    // Index "vidx" is NOT by expression, so ID matching is used to resolve column references.
    session.execute("CREATE INDEX vidx ON test_similar_column_name(v)");

    // Test case for covering-check using name resolution.
    // Index "jidx" uses json expression, so name-resolution is used to resolve column references.
    session.execute("CREATE INDEX jidx ON test_similar_column_name(j->'a'->>'b')" +
                    "  INCLUDE (v)");

    // Add "vvidx" to match with the original test case in bug report github #4881
    session.execute("CREATE INDEX vvidx ON test_similar_column_name(vv)");

    // Insert into table.
    int h = 7;
    int v = h * 2;
    int vv = h * 3;
    String jvalue = String.format("{ \"a\" : { \"b\" : \"bvalue_%d\" }," +
                                  "  \"a_column\" : %d }", h, h );
    String stmt = String.format("INSERT INTO test_similar_column_name(h, v, vv, j)" +
                                "  VALUES (%d, %d, %d, '%s');", h, v, vv, jvalue);
    session.execute(stmt);

    // Query using "vidx" to test ID resolution.
    String query = String.format("SELECT vv FROM test_similar_column_name WHERE v = %d;", v);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT vv FROM test_similar_column_name WHERE v = %d;", v * 2);
    assertEquals(0, session.execute(query).all().size());

    query = String.format("SELECT * FROM test_similar_column_name" +
                          "  WHERE v = %d AND vv = %d;", v, vv);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT * FROM test_similar_column_name" +
                          "  WHERE v = %d AND vv = %d;", v * 2, vv * 2);
    assertEquals(0, session.execute(query).all().size());

    query = String.format("SELECT v FROM test_similar_column_name" +
                          "  WHERE v = %d AND vv = %d;", v, vv);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT v FROM test_similar_column_name" +
                          "  WHERE v = %d AND vv = %d;", v * 2, vv * 2);
    assertEquals(0, session.execute(query).all().size());

    // Query using "jidx" to test NAME resolution.
    query = String.format("SELECT vv FROM test_similar_column_name" +
                          "  WHERE j->'a'->>'b' = 'bvalue_%d';", h);
    assertEquals(1, session.execute(query).all().size());

    query = String.format("SELECT vv FROM test_similar_column_name" +
                          "  WHERE j->'a'->>'b' = 'bvalue_%d';", h * 2);
    assertEquals(0, session.execute(query).all().size());
  }
}
