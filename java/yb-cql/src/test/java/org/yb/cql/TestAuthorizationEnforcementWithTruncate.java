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

import com.datastax.driver.core.PreparedStatement;
import com.datastax.driver.core.ResultSet;
import com.datastax.driver.core.Row;
import com.datastax.driver.core.Session;
import com.datastax.driver.core.exceptions.SyntaxError;
import com.datastax.driver.core.exceptions.UnauthorizedException;
import org.junit.*;
import org.junit.rules.TestName;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.YBTestRunner;
import org.yb.minicluster.BaseMiniClusterTest;

import java.sql.Time;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.fail;

// This test enables ycql_require_drop_privs_for_truncate flag.
@RunWith(value=YBTestRunner.class)
public class TestAuthorizationEnforcementWithTruncate extends TestAuthorizationEnforcement {
  private static final Logger LOG =
    LoggerFactory.getLogger(org.yb.cql.TestAuthorizationEnforcementWithTruncate.class);

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("ycql_require_drop_privs_for_truncate", Boolean.toString(true));
    return flagMap;
  }

  @Test
  public void testTruncateStatementWithWrongPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(DROP, CREATE, DESCRIBE), TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithWrongPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(DESCRIBE, DROP), KEYSPACE, keyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithDropPermissionOnDifferentTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    createTableAndInsertRecord(s, keyspace, anotherTable);
    grantPermission(DROP, TABLE, anotherTable, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithDropPermissionOnDifferentKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    createKeyspaceAndVerify(s, anotherKeyspace);
    grantPermission(DROP, KEYSPACE, anotherKeyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStamentWithDropPermissionOnTableToDifferentRole() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(DROP, TABLE, table, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithDropPermissionOnKeyspaceToDifferentRole()
      throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(DROP, KEYSPACE, keyspace, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(MODIFY, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithDropPermissionOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(DROP, TABLE, table, username);
    truncateTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(MODIFY, KEYSPACE, keyspace, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithDropPermissionOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(DROP, KEYSPACE, keyspace, username);
    truncateTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testTruncateStatementWithAllPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantAllPermission(TABLE, table, username);
    truncateTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testTruncateStatementWithAllPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantAllPermission(KEYSPACE, keyspace, username);
    truncateTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSuperuserCanTruncateTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));
    truncateTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testTruncateTableWithModifyPermission() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(MODIFY, TABLE, table, username);
    grantPermission(DROP, TABLE, table, username);

    // Prepare and excecute statement.
    String truncateStmt = String.format("TRUNCATE %s.%s", keyspace, table);
    PreparedStatement stmt = s2.prepare(truncateStmt);
    s2.execute(stmt.bind());

    revokePermission(DROP, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }
}
