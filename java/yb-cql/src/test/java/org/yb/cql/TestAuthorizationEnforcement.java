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

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.fail;

@RunWith(value=YBTestRunner.class)
public class TestAuthorizationEnforcement extends BaseAuthenticationCQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(org.yb.cql.TestRoles.class);

  private static final long PERMISSIONS_CACHE_TIME_MSECS = 100;

  // Time to sleep. Used to give the clients enough time to update their permissions cache.
  // Used only when revoking a permission or altering the role to remove superuser property.
  private static final long TIME_SLEEP_MS = PERMISSIONS_CACHE_TIME_MSECS * 4;

  // Value that we insert into the table.
  private static final int VALUE = 5;

  // Used for GRANT/REVOKE roles.
  private static final String GRANT = "grant";
  private static final String REVOKE = "revoke";

   // Session using 'cassandra' role.
  protected Session s = null;

  // Session using the created role.
  protected Session s2;

  protected String username;
  protected String anotherUsername;
  protected String password;
  protected String keyspace;
  protected String anotherKeyspace;
  protected String table;
  protected String anotherTable;

  @Rule
  public TestName testName = new TestName();

  @BeforeClass
  public static void SetUpBeforeClass() throws Exception {
    BaseMiniClusterTest.tserverArgs.add("--use_cassandra_authentication=true");
    BaseMiniClusterTest.tserverArgs.add("--update_permissions_cache_msecs=" +
                                        PERMISSIONS_CACHE_TIME_MSECS);
    BaseCQLTest.setUpBeforeClass();
  }

  @Before
  public void setupSession() throws Exception {
    if (s == null) {
      s = getDefaultSession();
    }

    String name = Integer.toString(Math.abs(testName.getMethodName().hashCode()));

    username = "role_" + name;
    password = "password_"+ name;
    testCreateRoleHelperWithSession(username, password, true, false, false, s);

    s2 = getSession(username, password);

    keyspace = "keyspace_" + name;
    table = "table_" + name;

    s.execute("CREATE KEYSPACE " + keyspace);

    anotherUsername = username + "_2";
    anotherKeyspace = keyspace + "_2";
    anotherTable = table + "_2";

    if (testName.getMethodName().startsWith("testGrantPermission") ||
        testName.getMethodName().startsWith("testRevokePermission")) {
      testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    }
  }

  @After
  public void cleanup() throws Exception {
    String name = Integer.toString(Math.abs(testName.getMethodName().hashCode()));
    s2.close();
    keyspace = "keyspace_" + name;

    // Get all the tables in keyspace if any.
    ResultSet rs = s.execute(String.format(
        "SELECT table_name FROM system_schema.tables WHERE keyspace_name = '%s'", keyspace));

    List<Row> tables = rs.all();
    // Delete all the tables.
    for (Row table : tables) {
      s.execute(String.format("DROP TABLE %s.%s", keyspace, table.getString("table_name")));
    }

    // Delete the keyspace.
    s.execute("DROP KEYSPACE IF EXISTS " + keyspace);
  }

  private List<String> getAllPermissionsExcept(List<String> exceptions) {
    List<String> permissions = new ArrayList<String>();
    for (String permission : ALL_PERMISSIONS) {
      if (!exceptions.contains(permission)) {
        permissions.add(permission);
      }
    }
    return permissions;
  }

  private void revokePermissionNoSleep(String permission, String resourceType, String resource,
                                       String role) throws Exception {
    s.execute(
        String.format("REVOKE %s ON %s %s FROM %s", permission, resourceType, resource,role));
  }

  protected void revokePermission(String permission, String resourceType, String resource,
                                String role) throws Exception {
    revokePermissionNoSleep(permission, resourceType, resource, role);
    Thread.sleep(TIME_SLEEP_MS);
  }

  protected void grantPermission(String permission, String resourceType, String resource,
                               String role) throws Exception {
    grantPermission(permission, resourceType, resource, role, s);
  }

  protected void grantAllPermissionsExcept(List<String> exceptions, String resourceType,
                                         String resource, String role) throws Exception {
    List<String> permissions = getAllPermissionsExcept(exceptions);
    for (String permission : permissions) {
      grantPermission(permission, resourceType, resource, role);
    }
  }

  protected void grantAllPermission(String resourceType, String resource, String role)
      throws Exception {
    grantPermission(ALL, resourceType, resource, role);
  }

  private void grantPermissionOnAllKeyspaces(String permission, String role) throws Exception {
    grantPermission(permission, ALL_KEYSPACES, "", role);
  }

  protected void revokePermissionOnAllKeyspaces(String permission, String role) throws Exception {
    revokePermission(permission, ALL_KEYSPACES, "", role);
  }

  private void grantPermissionOnAllRoles(String permission, String role) throws Exception {
    grantPermission(permission, ALL_ROLES, "", role);
  }

  private void verifySomePermissionsGranted(String role, String resource) {
    ResultSet rs = s.execute(String.format(
        "SELECT * FROM system_auth.role_permissions WHERE role = '%s' AND resource = '%s'",
        role, resource));
    assert(!rs.all().isEmpty());
  }

  private void verifyPermissionsDeleted(String role, String resource) {
    ResultSet rs = s.execute(String.format(
        "SELECT * FROM system_auth.role_permissions WHERE role = '%s' AND resource = '%s'",
        role, resource));
    assert(rs.all().isEmpty());
  }

  private void verifyKeyspaceExists(String keyspaceName) throws Exception {
    ResultSet rs = s.execute(String.format(
        "SELECT * FROM system_schema.keyspaces WHERE keyspace_name = '%s'", keyspaceName));
    List<Row> list = rs.all();
    assertEquals(1, list.size());
  }

  protected void createKeyspaceAndVerify(Session session, String keyspaceName) throws Exception {
    session.execute("CREATE KEYSPACE " + keyspaceName);
    verifyKeyspaceExists(keyspaceName);
  }

  private void deleteKeyspaceAndVerify(Session session, String keyspaceName) throws Exception {
    verifyKeyspaceExists(keyspaceName);

    session.execute("DROP KEYSPACE " + keyspaceName);

    ResultSet rs = s.execute(String.format(
        "SELECT * FROM system_schema.keyspaces WHERE keyspace_name = '%s'", keyspaceName));
    List<Row> list = rs.all();
    assertEquals(0, list.size());
  }

  private void verifyTableExists(String keyspaceName, String tableName) {
    // Verify that the table was created.
    ResultSet rs = s.execute(String.format(
        "SELECT * FROM system_schema.tables WHERE keyspace_name = '%s' AND table_name = '%s'",
        keyspaceName, tableName));

    List<Row> list = rs.all();
    assertEquals(1, list.size());
  }

  protected void createTableAndVerify(Session session, String keyspaceName, String tableName)
      throws Exception {
    // Now, username should be able to create the table.
    session.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h))",
        keyspaceName, tableName));

    s.execute("USE " + keyspaceName);
    verifyTableExists(keyspaceName, tableName);
  }

  private void deleteTableAndVerify(Session session, String keyspaceName, String tableName)
    throws Exception {
    verifyTableExists(keyspaceName, tableName);
    session.execute(String.format("DROP TABLE %s.%s ", keyspaceName, tableName));

    ResultSet rs = s.execute(String.format(
        "SELECT * FROM system_schema.tables WHERE keyspace_name = '%s' AND table_name = '%s'",
        keyspaceName, tableName));

    List<Row> list = rs.all();
    assertEquals(0, list.size());
  }

  private void verifyRow(Session session, String keyspaceName, String tableName, int expectedValue)
      throws Exception {

    ResultSet rs = session.execute(String.format("SELECT * FROM %s.%s", keyspaceName, table));
    List<Row> rows = rs.all();
    assertEquals(1, rows.size());
    assertEquals(VALUE, rows.get(0).getInt("h"));
    assertEquals(expectedValue, rows.get(0).getInt("v"));
  }

  private void selectAndVerify(Session session, String keyspaceName, String tableName)
    throws Exception {
    verifyRow(session, keyspaceName, tableName, VALUE);
  }

  private void insertRow(Session session, String keyspaceName, String tableName)
    throws Exception {

    session.execute(String.format("INSERT INTO %s.%s (h, v) VALUES (%d, %d)",
        keyspaceName, tableName, VALUE, VALUE));

    // We always verify by using the cassandra role.
    selectAndVerify(s, keyspaceName, tableName);
  }

  private void updateRowAndVerify(Session session, String keyspaceName, String tableName)
    throws Exception {

    session.execute(String.format("UPDATE %s.%s SET v = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));

    verifyRow(s, keyspaceName, tableName, VALUE + 1);
  }

  protected void truncateTableAndVerify(Session session, String keyspaceName, String tableName)
      throws Exception {
    s2.execute(String.format("TRUNCATE %s.%s", keyspaceName, tableName));

    ResultSet rs = s.execute(String.format("SELECT * FROM %s.%s", keyspaceName, tableName));
    assertEquals(0, rs.all().size());
  }

  protected void createTableAndInsertRecord(Session session, String keyspaceName, String tableName)
      throws Exception {
    createTableAndVerify(session, keyspaceName, tableName);
    insertRow(session, keyspaceName, tableName);
  }

  @Test
  public void testCreateKeyspaceWithoutPermissions() throws Exception {
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("CREATE KEYSPACE %s_2", keyspace));
  }

  @Test
  public void testCreateKeyspaceWithWrongPermissions() throws Exception {
    // Grant all the permissions except CREATE.
    grantAllPermissionsExcept(Arrays.asList(CREATE, DESCRIBE, AUTHORIZE),
        ALL_KEYSPACES, "", username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("CREATE KEYSPACE %s_2", keyspace));
  }

  @Test
  public void testCreateKeyspaceWithCreatePermission() throws Exception {
    // Grant CREATE permission.
    grantPermissionOnAllKeyspaces(CREATE, username);

    createKeyspaceAndVerify(s2, keyspace + "_2");
  }

  @Test
  public void testCreateKeyspaceWithAllPermissions() throws Exception {
    // Grant ALL permissions.
    grantPermissionOnAllKeyspaces(ALL, username);

    createKeyspaceAndVerify(s2, keyspace + "_2");
  }

  @Test
  public void testSuperuserCanCreateKeyspace() throws Exception {
    // Make the role a superuser.
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    createKeyspaceAndVerify(s2, keyspace + "_2");
  }

  @Test
  public void testDeleteKeyspaceWithNoPermissions() throws Exception {
    thrown.expect(UnauthorizedException.class);
    s2.execute("DROP KEYSPACE " + keyspace);
  }

  @Test
  public void testDeleteKeyspaceWithWrongPermissions() throws Exception {
    grantAllPermissionsExcept(Arrays.asList(DROP, DESCRIBE, AUTHORIZE), KEYSPACE, keyspace,
        username);

    thrown.expect(UnauthorizedException.class);
    s2.execute("DROP KEYSPACE " + keyspace);
  }

  @Test
  public void testDeleteKeyspaceWithDropPermissionOnDifferentKeyspace() throws Exception {
    createKeyspaceAndVerify(s, anotherKeyspace);

    grantPermission(DROP, KEYSPACE, anotherKeyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute("DROP KEYSPACE " + keyspace);
  }

  @Test
  public void testDeleteKeyspaceWithDropPermission() throws Exception {
    // Grant DROP permission on this test's keyspace.
    grantPermission(DROP, KEYSPACE, keyspace, username);

    deleteKeyspaceAndVerify(s2, keyspace);
  }

  @Test
  public void testDeleteKeyspaceWithDropPermissionOnAllKeyspaces() throws Exception {
    // Grant DROP permission on all keyspaces.
    grantPermissionOnAllKeyspaces(DROP, username);

    deleteKeyspaceAndVerify(s2, keyspace);
  }

  @Test
  public void testSuperuserCanDeleteKeyspace() throws Exception {
    // Make the role a superuser.
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    deleteKeyspaceAndVerify(s2, keyspace);
  }

  @Test
  public void testCreateTableWithoutPermissions() throws Exception {
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("CREATE table %s.%s (h int, primary key(h))", keyspace, table));
  }

  @Test
  public void testCreateTableWithWrongPermissions() throws Exception {
    // Grant all the permissions except CREATE.
    grantAllPermissionsExcept(Arrays.asList(CREATE, DESCRIBE, AUTHORIZE), KEYSPACE, keyspace,
        username);

    // username shouldn't be able to create a table.
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("CREATE table %s.%s (h int, primary key(h))", keyspace, table));
  }

  @Test
  public void testCreateTableWithCreatePermission() throws Exception {
    // Grant CREATE permission on the keyspace.
    grantPermission(CREATE, KEYSPACE, keyspace, username);

    createTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSuperuserCanCreateTable() throws Exception {
    // Make the role a superuser.
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    createTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testDeleteTableWithNoPermissions() throws Exception {
    createTableAndVerify(s, keyspace, table);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("DROP TABLE %s.%s", keyspace, table));
  }

  @Test
  public void testDeleteTableWithWrongPermissions() throws Exception {
    createTableAndVerify(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(CREATE, DROP, DESCRIBE, AUTHORIZE), TABLE,
        keyspace + "." + table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("DROP TABLE %s.%s", keyspace, table));
  }

  @Test
  public void testDeleteTableWithDropPermissionOnDifferentKeyspace() throws Exception {
    createTableAndVerify(s, keyspace, table);

    createKeyspaceAndVerify(s, anotherKeyspace);

    grantPermission(DROP, KEYSPACE, anotherKeyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("DROP TABLE %s.%s", keyspace, table));
  }

  @Test
  public void testDeleteTableWithDropPermissionOnDifferentTable() throws Exception {
    createTableAndVerify(s, keyspace, table);

    createTableAndVerify(s, keyspace, anotherTable);
    grantPermission(DROP, TABLE, keyspace + "." + anotherTable, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("DROP TABLE %s.%s", keyspace, table));
  }

  @Test
  public void testDeleteTableWithDropPermissionOnKeyspace() throws Exception {
    createTableAndVerify(s, keyspace, table);

    // Grant DROP permission on this test's keyspace.
    grantPermission(DROP, KEYSPACE, keyspace, username);

    deleteTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testDeleteTableWithDropPermissionOnTable() throws Exception {
    createTableAndVerify(s, keyspace, table);

    // Grant DROP permission on this test's keyspace.
    grantPermission(DROP, TABLE, table, username);

    deleteTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testDeleteTableWithDropPermissionOnAllKeyspaces() throws Exception {
    createTableAndVerify(s, keyspace, table);

    // Grant DROP permission on all keyspaces.
    grantPermissionOnAllKeyspaces(DROP, username);

    deleteTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSuperuserCanDeleteTable() throws Exception {
    createTableAndVerify(s, keyspace, table);

    // Make the role a superuser.
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    deleteTableAndVerify(s2, keyspace, table);
  }

  private void testStatementWithNoPermissions() throws Exception {

  }

  /*
   * SELECT statements tests.
   */

  @Test
  public void testSelectStatementWithNoPermissions() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithWrongPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(SELECT, CREATE, DESCRIBE), TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithWrongPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(SELECT, DESCRIBE), KEYSPACE, keyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithSelectPermissionOnDifferentTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    String table2 = table + "_2";
    createTableAndInsertRecord(s, keyspace, table2);

    grantPermission(SELECT, TABLE, table2, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithSelectPermissionOnDifferentKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    String keyspace2 = keyspace + "_2";

    s.execute("CREATE KEYSPACE " + keyspace2);
    grantPermission(SELECT, KEYSPACE, keyspace2, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithSelectPermissionOnTableToDifferentRole() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(SELECT, TABLE, table, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithSelectPermissionOnKeyspaceToDifferentRole() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(SELECT, KEYSPACE, keyspace, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("SELECT * from %s.%s", keyspace, table));
  }

  @Test
  public void testSelectStatementWithSelectPermissionOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(SELECT, TABLE, table, username);

    selectAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSelectStatementWithSelectPermissionOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(SELECT, KEYSPACE, keyspace, username);

    selectAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSelectStatementWithAllPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantAllPermission(TABLE, table, username);
    selectAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSelectStatementWithAllPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantAllPermission(KEYSPACE, keyspace, username);
    selectAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSuperuserCanSelectFromTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    // Make the role a superuser.
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    selectAndVerify(s2, keyspace, table);
  }

  /*
   * INSERT statements tests.
   */

  @Test
  public void testInsertStatementWithNoPermissions() throws Exception {
    createTableAndVerify(s, keyspace, table);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithWrongPermissionsOnTable() throws Exception {
    createTableAndVerify(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(MODIFY, CREATE, DESCRIBE), TABLE, table, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithWrongPermissionsOnKeyspace() throws Exception {
    createTableAndVerify(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(MODIFY, DESCRIBE), KEYSPACE, keyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithModifyPermissionOnDifferentTable() throws Exception {
    createTableAndVerify(s, keyspace, table);

    String table2 = table + "_2";
    createTableAndVerify(s, keyspace, table2);

    grantPermission(MODIFY, TABLE, table2, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithModifyPermissionOnDifferentKeyspace() throws Exception {
    createTableAndVerify(s, keyspace, table);

    String keyspace2 = keyspace + "_2";
    s.execute("CREATE KEYSPACE " + keyspace2);
    grantPermission(MODIFY, KEYSPACE, keyspace2, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithModifyPermissionOnTableToDifferentRole() throws Exception {
    createTableAndVerify(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(MODIFY, TABLE, table, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithModifyPermissionOnKeyspaceToDifferentRole() throws Exception {
    createTableAndVerify(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(MODIFY, KEYSPACE, keyspace, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("INSERT INTO %s.%s (h) VALUES (%d)", keyspace, table, VALUE));
  }

  @Test
  public void testInsertStatementWithModifyPermissionOnTable() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(MODIFY, TABLE, table, username);
    insertRow(s2, keyspace, table);
  }

  @Test
  public void testInsertStatementWithModifyPermissionOnKeyspace() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(MODIFY, KEYSPACE, keyspace, username);
    insertRow(s2, keyspace, table);
  }

  @Test
  public void testInsertStatementWithAllPermissionsOnTable() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantAllPermission(TABLE, table, username);
    insertRow(s2, keyspace, table);
  }

  @Test
  public void testInsertStatementWithAllPermissionsOnKeyspace() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantAllPermission(KEYSPACE, keyspace, username);
    insertRow(s2, keyspace, table);
  }

  @Test
  public void testSuperuserCanInsertIntoTable() throws Exception {
    createTableAndVerify(s, keyspace, table);

    // Make the role a superuser.
    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    insertRow(s2, keyspace, table);
  }

  /*
   * UPDDATE statements tests.
   */

  @Test
  public void testUpdateStatementWithNoPermissions() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithWrongPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(MODIFY, CREATE, DESCRIBE), TABLE, table, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithWrongPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(DESCRIBE, MODIFY), KEYSPACE, keyspace, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithModifyPermissionOnDifferentTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    createTableAndVerify(s, keyspace, anotherTable);
    grantPermission(MODIFY, TABLE, anotherTable, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithModifyPermissionOnDifferentKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    createKeyspaceAndVerify(s, anotherKeyspace);
    grantPermission(MODIFY, KEYSPACE, anotherKeyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithModifyPermissionOnTableToDifferentRole() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(MODIFY, TABLE, table, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithModifyPermissionOnKeyspaceToDifferentRole() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(MODIFY, KEYSPACE, keyspace, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("UPDATE %s.%s SET h = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));
  }

  @Test
  public void testUpdateStatementWithModifyPermissionOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantPermission(MODIFY, TABLE, table, username);
    updateRowAndVerify(s2, keyspace, table);
  }

  @Test
  public void testUpdateStatementWithModifyPermissionOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantPermission(MODIFY, KEYSPACE, keyspace, username);
    updateRowAndVerify(s2, keyspace, table);
  }

  @Test
  public void testUpdateStatementWithAllPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermission(TABLE, table, username);
    updateRowAndVerify(s2, keyspace, table);
  }

  @Test
  public void testUpdateStatementWithAllPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermission(KEYSPACE, keyspace, username);
    updateRowAndVerify(s2, keyspace, table);
  }

  @Test
  public void testSuperuserCanUpdateTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    s.execute(String.format("ALTER ROLE %s with SUPERUSER = true", username));

    updateRowAndVerify(s2, keyspace, table);
  }

   /*
   * TRUNCATE statements tests.
   */

  @Test
  public void testTruncateStatementWithNoPermissions() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithWrongPermissionsOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(MODIFY, CREATE, DESCRIBE), TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithWrongPermissionsOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantAllPermissionsExcept(Arrays.asList(DESCRIBE, MODIFY), KEYSPACE, keyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnDifferentTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    createTableAndInsertRecord(s, keyspace, anotherTable);
    grantPermission(MODIFY, TABLE, anotherTable, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnDifferentKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    createKeyspaceAndVerify(s, anotherKeyspace);
    grantPermission(MODIFY, KEYSPACE, anotherKeyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStamentWithModifyPermissionOnTableToDifferentRole() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(MODIFY, TABLE, table, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnKeyspaceToDifferentRole()
      throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    testCreateRoleHelperWithSession(anotherUsername, password, true, false, false, s);
    grantPermission(MODIFY, KEYSPACE, keyspace, anotherUsername);

    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("TRUNCATE %s.%s", keyspace, table));
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnTable() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(MODIFY, TABLE, table, username);
    truncateTableAndVerify(s2, keyspace, table);
  }

  @Test
  public void testTruncateStatementWithModifyPermissionOnKeyspace() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);
    grantPermission(MODIFY, KEYSPACE, keyspace, username);
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

  /*
   * Grant or Revoke test helper methods.
   */

  private void testGrantRevokeRoleWithoutPermissions(String stmtType) throws Exception {
    String r = String.format("%s_role_no_permissions", stmtType);
    testCreateRoleHelperWithSession(r, password, false, false, false, s);

    thrown.expect(UnauthorizedException.class);
    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", r, anotherUsername));
    } else {
      s2.execute(String.format("REVOKE %s FROM %s", r, anotherUsername));
    }
  }

  private void testGrantRevokeRoleWithoutPermissionOnRecipientRole(String stmtType)
      throws Exception {
    String grantedRole = String.format("%s_role_without_permissions_on_recipient", stmtType);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    // Grant AUTHORIZE on grantedRole.
    grantPermission(AUTHORIZE, ROLE, grantedRole, username);

    thrown.expect(UnauthorizedException.class);
    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", grantedRole, anotherUsername));
    } else {
      s2.execute(String.format("REVOKE %s FROM %s", grantedRole, anotherUsername));
    }
  }

  private void testGrantRevokeRoleWithoutPermissionOnGrantedRole(String stmtType) throws Exception {
    String recipientRole = String.format("%s_without_permissions_on_granted", stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);

    // Grant AUTHORIZE on recipientRole */
    grantPermission(AUTHORIZE, ROLE, recipientRole, username);

    thrown.expect(UnauthorizedException.class);
    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", anotherUsername, recipientRole));
    } else {
      s2.execute(String.format("REVOKE %s FROM %s", anotherUsername, recipientRole));
    }
  }

  private void testGrantRevokeRoleWithWrongPermissionsOnGrantedAndRecipientRoles(String stmtType)
      throws Exception {
    String recipientRole = String.format("%s_recipient_role_wrong_permissions", stmtType);
    String grantedRole = String.format("%s_granted_role_wrong_permissions", stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, CREATE, DESCRIBE, MODIFY, SELECT),
        ROLE, grantedRole, username);
    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, CREATE, DESCRIBE, MODIFY, SELECT),
        ROLE, recipientRole, username);

    thrown.expect(UnauthorizedException.class);
    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));
    } else {
      s2.execute(String.format("revoke %s FROM %s", grantedRole, recipientRole));
    }
  }

  private void testGrantRevokeRoleWithWrongPermissionsOnAllRoles(String stmtType) throws Exception {
    String recipientRole = String.format("%s_recipient_role_wrong_permissions_on_roles", stmtType);
    String grantedRole = String.format("%s_granted_role_wrong_permissions_on_roles", stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, MODIFY, SELECT), ALL_ROLES, "", username);

    thrown.expect(UnauthorizedException.class);
    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));
    } else {
      s2.execute(String.format("REVOKE %s FROM %s", grantedRole, recipientRole));
    }
  }

  private void testGrantRevokeRoleWithPermissionOnGrantedAndRecipientRoles(String stmtType)
      throws Exception {
    String recipientRole = String.format("%s_recipient_role_full_permissions", stmtType);
    String grantedRole = String.format("%s_granted_role_full_permissions", stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    grantPermission(AUTHORIZE, ROLE, grantedRole, username);
    grantPermission(AUTHORIZE, ROLE, recipientRole, username);

    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));
    } else {
      // Grant the role first using cassandra role.
      s.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));
      s2.execute(String.format("REVOKE %s FROM %s", grantedRole, recipientRole));
    }
  }

  private void testGrantRevokeRoleWithPermissionOnAllRoles(String stmtType) throws Exception {
    String recipientRole = String.format("%s_recipient_role_full_permissions_on_roles", stmtType);
    String grantedRole = String.format("%s_granted_role_full_permissions_on_roles", stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    grantPermissionOnAllRoles(AUTHORIZE, username);

    if (stmtType.equals(GRANT)) {
      s2.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));
    } else {
      // Grant the role first using cassandra role.
      s.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));
      s2.execute(String.format("REVOKE %s FROM %s", grantedRole, recipientRole));
    }
  }

  //
  // GRANT ROLE statements
  //

  @Test
  public void testGrantRoleWithoutPermissions() throws Exception {
    testGrantRevokeRoleWithoutPermissions(GRANT);
  }

  // AUTHORIZE permission only on the granted role.
  @Test
  public void testGrantRoleWithoutPermissionOnRecipientRole() throws Exception {
    testGrantRevokeRoleWithoutPermissionOnRecipientRole(GRANT);
  }

  // AUTHORIZE permission only on the recipient role.
  @Test
  public void testGrantRoleWithoutPermissionOnGrantedRole() throws Exception {
    testGrantRevokeRoleWithoutPermissionOnGrantedRole(GRANT);
  }

  @Test
  public void testGrantRoleWithWrongPermissionsOnGrantedAndRecipientRoles() throws Exception {
    testGrantRevokeRoleWithWrongPermissionsOnGrantedAndRecipientRoles(GRANT);
  }

  @Test
  public void testGrantRoleWithWrongPermissionsOnAllRoles() throws Exception {
    testGrantRevokeRoleWithWrongPermissionsOnAllRoles(GRANT);
  }

  // AUTHORIZE permission only on the recipient and granted roles.
  @Test
  public void testGrantRoleWithPermissionOnGrantedAndRecipientRoles() throws Exception {
    testGrantRevokeRoleWithPermissionOnGrantedAndRecipientRoles(GRANT);
  }

  // AUTHORIZE permission only on ALL ROLES.
  @Test
  public void testGrantRoleWithPermissionOnALLRoles() throws Exception {
    testGrantRevokeRoleWithPermissionOnAllRoles(GRANT);
  }

  //
  // REVOKE ROLE statements
  //

  @Test
  public void testRevokeRoleWithoutPermissions() throws Exception {
    testGrantRevokeRoleWithoutPermissions(REVOKE);
  }

  // AUTHORIZE permission only on the granted role.
  @Test
  public void testRevokeRoleWithoutPermissionOnRecipientRole() throws Exception {
    testGrantRevokeRoleWithoutPermissionOnRecipientRole(REVOKE);
  }

  // AUTHORIZE permission only on the recipient role.
  @Test
  public void testRevokeRoleWithoutPermissionOnRevokeedRole() throws Exception {
    testGrantRevokeRoleWithoutPermissionOnGrantedRole(REVOKE);
  }

  @Test
  public void testRevokeRoleWithWrongPermissionsOnGrantedAndRecipientRoles() throws Exception {
    testGrantRevokeRoleWithWrongPermissionsOnGrantedAndRecipientRoles(REVOKE);
  }

  @Test
  public void testRevokeRoleWithWrongPermissionsOnAllRoles() throws Exception {
    testGrantRevokeRoleWithWrongPermissionsOnAllRoles(REVOKE);
  }

  // AUTHORIZE permission only on the recipient and granted roles.
  @Test
  public void testRevokeRoleWithPermissionOnGrantedAndRecipientRoles() throws Exception {
    testGrantRevokeRoleWithPermissionOnGrantedAndRecipientRoles(REVOKE);
  }

  // AUTHORIZE permission only on ALL ROLES.
  @Test
  public void testRevokeRoleWithPermissionOnALLRoles() throws Exception {
    testGrantRevokeRoleWithPermissionOnAllRoles(REVOKE);
  }

  //
  // Grant/Revoke permissions on keyspaces/tables helper methods.
  //
  private String getGrantOnKeyspaceStmt() {
    return String.format("GRANT CREATE ON KEYSPACE %s TO %s", keyspace, anotherUsername);
  }

  private String getRevokeFromKeyspaceStmt() {
    return String.format("REVOKE CREATE ON KEYSPACE %s FROM %s", keyspace, anotherUsername);
  }

  private void grantAuthorizePermissionOnKeyspace() throws Exception {
    s.execute(getGrantOnKeyspaceStmt());
  }

  private void testGrantAuthorizePermissionOnKeyspaceFails() throws Exception {
    thrown.expect(UnauthorizedException.class);
    s2.execute(getGrantOnKeyspaceStmt());
  }

  private void testRevokeAuthorizePermissionFromKeyspaceFails() throws Exception {
    // First grant the permission using cassandra role.
    grantAuthorizePermissionOnKeyspace();
    thrown.expect(UnauthorizedException.class);
    s2.execute(getRevokeFromKeyspaceStmt());
  }

  private void testGrantRevokePermissionOnKeyspaceWithNoPermissions(String stmtType)
      throws Exception {
    if (stmtType.equals(GRANT)) {
      testGrantAuthorizePermissionOnKeyspaceFails();
    } else {
      testRevokeAuthorizePermissionFromKeyspaceFails();
    }
  }

  private void testGrantRevokePermissionOnKeyspaceWithWrongPermissionsOnKeyspace(String stmtType)
      throws Exception {
    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, DESCRIBE), KEYSPACE, keyspace, username);
    if (stmtType.equals(GRANT)) {
      testGrantAuthorizePermissionOnKeyspaceFails();
    } else {
      testRevokeAuthorizePermissionFromKeyspaceFails();
    }
  }

  private void testGrantRevokePermissionOnKeyspaceWithWrongPermissionsOnAllKeyspaces(
      String stmtType) throws Exception {
    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, DESCRIBE), ALL_KEYSPACES, "", username);
    if (stmtType.equals(GRANT)) {
      testGrantAuthorizePermissionOnKeyspaceFails();
    } else {
      testRevokeAuthorizePermissionFromKeyspaceFails();
    }
  }

  private void testGrantRevokePermissionOnKeyspaceWithAuthorizePermissionOnKeyspace(String stmtType)
      throws Exception {
    grantPermission(AUTHORIZE, KEYSPACE, keyspace, username);
    if (stmtType.equals(GRANT)) {
      s2.execute(getGrantOnKeyspaceStmt());
    } else {
      s.execute(getGrantOnKeyspaceStmt());
      s2.execute(getRevokeFromKeyspaceStmt());
    }
  }

  private void testGrantRevokePermissionOnKeyspaceWithAuthorizePermissionOnAllKeyspaces(
      String stmtType) throws Exception {
    grantPermissionOnAllKeyspaces(AUTHORIZE, username);
    if (stmtType.equals(GRANT)) {
      s2.execute(getGrantOnKeyspaceStmt());
    } else {
      s.execute(getGrantOnKeyspaceStmt());
      s2.execute(getRevokeFromKeyspaceStmt());
    }
  }

  private String getGrantOnTableStmt() {
    return String.format("GRANT SELECT ON TABLE %s.%s TO %s", keyspace, table, anotherUsername);
  }

  private String getRevokeFromTableStmt() {
    return String.format("REVOKE SELECT ON TABLE %s.%s FROM %s", keyspace, table,
        anotherUsername);
  }

  private void testGrantPermissionOnTableFails() throws Exception {
    thrown.expect(UnauthorizedException.class);
    s2.execute(getGrantOnTableStmt());
  }

  private void testRevokePermissionOnTableFails() throws Exception {
    // First grant the permission using cassandra role.
    s.execute(getGrantOnTableStmt());
    thrown.expect(UnauthorizedException.class);
    s2.execute(getRevokeFromTableStmt());
  }

  private void testGrantRevokePermissionOnTableWithNoPermissions(String stmtType) throws Exception {
    createTableAndVerify(s, keyspace, table);
    if (stmtType.equals(GRANT)) {
      testGrantPermissionOnTableFails();
    } else {
      testRevokePermissionOnTableFails();
    }
  }

  private void testGrantRevokePermissionOnTableWithWrongPermissionsOnTable(String stmtType)
      throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, CREATE, DESCRIBE), TABLE, table, username);
    if (stmtType.equals(GRANT)) {
      testGrantPermissionOnTableFails();
    } else {
      testRevokePermissionOnTableFails();
    }
  }

  private void testGrantRevokePermissionOnTableWithWrongPermissionsOnKeyspace(String stmtType)
      throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, DESCRIBE), KEYSPACE, keyspace, username);
    if (stmtType.equals(GRANT)) {
      testGrantPermissionOnTableFails();
    } else {
      testRevokePermissionOnTableFails();
    }
  }

  private void testGrantRevokePermissionOnTableWithWrongPermissionsOnAllKeyspaces(String stmtType)
      throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantAllPermissionsExcept(Arrays.asList(AUTHORIZE, DESCRIBE), ALL_KEYSPACES, "", username);
    if (stmtType.equals(GRANT)) {
      testGrantPermissionOnTableFails();
    } else {
      testRevokePermissionOnTableFails();
    }
  }

  private void testGrantRevokePermissionOnTableWithAuthorizePermissionOnTable(String stmtType)
      throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(AUTHORIZE, TABLE, table, username);
    if (stmtType.equals(GRANT)) {
      s2.execute(getGrantOnTableStmt());
    } else {
      // First grant the permission using cassandra role.
      s.execute(getGrantOnTableStmt());
      s2.execute(getRevokeFromTableStmt());
    }
  }

  private void testGrantRevokePermissionOnTableWithAuthorizePermissionOnKeyspace(String stmtType)
      throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(AUTHORIZE, KEYSPACE, keyspace, username);
    if (stmtType.equals(GRANT)) {
      s2.execute(getGrantOnTableStmt());
    } else {
      // First grant the permission using cassandra role.
      s.execute(getGrantOnTableStmt());
      s2.execute(getRevokeFromTableStmt());
    }
  }

  private void testGrantRevokePermissionOnTableWithAuthorizePermissionOnAllKeyspaces(
      String stmtType) throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(AUTHORIZE, ALL_KEYSPACES, "", username);
    if (stmtType.equals(GRANT)) {
      s2.execute(getGrantOnTableStmt());
    } else {
      // First grant the permission using cassandra role.
      s.execute(getGrantOnTableStmt());
      s2.execute(getRevokeFromTableStmt());
    }
  }

  //
  // GRANT PERMISSION statements.
  //

  @Test
  public void testGrantPermissionOnKeyspaceWithNoPermissions() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithNoPermissions(GRANT);
  }

  @Test
  public void testGrantPermissionOnKeyspaceWithWrongPermissionsOnKeyspace() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithWrongPermissionsOnKeyspace(GRANT);
  }

  @Test
  public void testGrantPermissionOnKeyspaceWithWrongPermissionsOnAllKeyspaces() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithWrongPermissionsOnAllKeyspaces(GRANT);
  }

  @Test
  public void testGrantPermissionOnKeyspaceWithAuthorizePermissionOnKeyspace() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithAuthorizePermissionOnKeyspace(GRANT);
  }

  @Test
  public void testGrantPermissionOnKeyspaceWithAuthorizePermissionOnAllKeyspaces()
      throws Exception {
    testGrantRevokePermissionOnKeyspaceWithAuthorizePermissionOnAllKeyspaces(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithNoPermissions() throws Exception {
    testGrantRevokePermissionOnTableWithNoPermissions(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithWrongPermissionsOnTable() throws Exception {
    testGrantRevokePermissionOnTableWithWrongPermissionsOnTable(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithWrongPermissionsOnKeyspace() throws Exception {
    testGrantRevokePermissionOnTableWithWrongPermissionsOnKeyspace(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithWrongPermissionsOnAllKeyspaces() throws Exception {
    testGrantRevokePermissionOnTableWithWrongPermissionsOnAllKeyspaces(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithAuthorizePermissionOnTable() throws Exception {
    testGrantRevokePermissionOnTableWithAuthorizePermissionOnTable(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithAuthorizePermissionOnKeyspace() throws Exception {
    testGrantRevokePermissionOnTableWithAuthorizePermissionOnKeyspace(GRANT);
  }

  @Test
  public void testGrantPermissionOnTableWithAuthorizePermissionOnAllKeyspaces() throws Exception {
    testGrantRevokePermissionOnTableWithAuthorizePermissionOnAllKeyspaces(GRANT);
  }

  //
  // REVOKE PERMISSION statements.
  //

  @Test
  public void testRevokePermissionOnKeyspaceWithNoPermissions() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithNoPermissions(REVOKE);
  }

  @Test
  public void testRevokePermissionOnKeyspaceWithWrongPermissionsOnKeyspace() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithWrongPermissionsOnKeyspace(REVOKE);
  }

  @Test
  public void testRevokePermissionOnKeyspaceWithWrongPermissionsOnAllKeyspaces() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithWrongPermissionsOnAllKeyspaces(REVOKE);
  }

  @Test
  public void testRevokePermissionOnKeyspaceWithAuthorizePermissionOnKeyspace() throws Exception {
    testGrantRevokePermissionOnKeyspaceWithAuthorizePermissionOnKeyspace(REVOKE);
  }

  @Test
  public void testRevokePermissionOnKeyspaceWithAuthorizePermissionOnAllKeyspaces()
      throws Exception {
    testGrantRevokePermissionOnKeyspaceWithAuthorizePermissionOnAllKeyspaces(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithNoPermissions() throws Exception {
    testGrantRevokePermissionOnTableWithNoPermissions(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithWrongPermissionsOnTable() throws Exception {
    testGrantRevokePermissionOnTableWithWrongPermissionsOnTable(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithWrongPermissionsOnKeyspace() throws Exception {
    testGrantRevokePermissionOnTableWithWrongPermissionsOnKeyspace(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithWrongPermissionsOnAllKeyspaces() throws Exception {
    testGrantRevokePermissionOnTableWithWrongPermissionsOnAllKeyspaces(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithAuthorizePermissionOnTable() throws Exception {
    testGrantRevokePermissionOnTableWithAuthorizePermissionOnTable(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithAuthorizePermissionOnKeyspace() throws Exception {
    testGrantRevokePermissionOnTableWithAuthorizePermissionOnKeyspace(REVOKE);
  }

  @Test
  public void testRevokePermissionOnTableWithAuthorizePermissionOnAllKeyspaces() throws Exception {
    testGrantRevokePermissionOnTableWithAuthorizePermissionOnAllKeyspaces(REVOKE);
  }

  @Test
  public void testPreparedCreateKeyspaceWithCreatePermission() throws Exception {
    grantPermissionOnAllKeyspaces(CREATE, username);

    // Prepare and execute statement.
    String createKeyspaceStmt = "CREATE KEYSPACE prepared_keyspace";
    PreparedStatement stmt = s2.prepare(createKeyspaceStmt);
    s2.execute(stmt.bind());

    revokePermission(CREATE, ALL_KEYSPACES, "", username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedCreateTableWithCreatePermission() throws Exception {
    grantPermission(CREATE, KEYSPACE, keyspace, username);

    s2.execute("USE " + keyspace);
    // Prepare and execute statement.
    String createTableStmt = String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h))",
        keyspace, "prepared_table");
    PreparedStatement stmt = s2.prepare(createTableStmt);
    s2.execute(stmt.bind());

    revokePermission(CREATE, KEYSPACE, keyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedAlterTableWithAlterPermission() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(ALTER, TABLE, table, username);

    // Prepare and execute statement.
    String alterTableStmt = String.format("ALTER TABLE %s.%s ADD v2 int", keyspace, table);
    PreparedStatement stmt = s2.prepare(alterTableStmt);
    s2.execute(stmt.bind());

    revokePermission(ALTER, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testTruncateTableWithModifyPermission() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(MODIFY, TABLE, table, username);

    // Prepare and excecute statement.
    String truncateStmt = String.format("TRUNCATE %s.%s", keyspace, table);
    PreparedStatement stmt = s2.prepare(truncateStmt);
    s2.execute(stmt.bind());

    revokePermission(MODIFY, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedInsertStmtWithSuperuserRole() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, password, true, true, false, s);
    Session s3 = getSession(anotherUsername, password);

    createTableAndVerify(s, keyspace, table);

    // Prepare and execute statement.
    String insertStmt = String.format("INSERT INTO %s.%s (h, v) VALUES (?, ?)", keyspace, table);
    PreparedStatement stmt = s3.prepare(insertStmt);

    ResultSet rs = s3.execute(stmt.bind(3, 5));

    s.execute(String.format("ALTER ROLE %s with SUPERUSER = false", anotherUsername));
    Thread.sleep(TIME_SLEEP_MS);

    thrown.expect(UnauthorizedException.class);
    rs = s3.execute(stmt.bind(4, 2));
  }

  @Test
  public void testPreparedInsertStmtWithModifyPermission() throws Exception {
    createTableAndVerify(s, keyspace, table);

    grantPermission(MODIFY, TABLE, table, username);

    // Prepare and execute statement.
    String insertStmt = String.format("INSERT INTO %s.%s (h, v) VALUES (?, ?)", keyspace, table);
    PreparedStatement stmt = s2.prepare(insertStmt);

    ResultSet rs = s2.execute(stmt.bind(3, 5));

    // Revoke the MODIFY permissions so the next execution of the prepared statement fails.
    revokePermission(MODIFY, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    rs = s2.execute(stmt.bind(4, 2));
  }

  @Test
  public void testPreparedSelectStmtWithSelectPermission() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantPermission(SELECT, TABLE, table, username);

    // Prepare and execute statement.
    String selectStmt = String.format("SELECT * FROM %s.%s", keyspace, table);
    PreparedStatement stmt = s2.prepare(selectStmt);

    ResultSet rs = s2.execute(stmt.bind());
    List<Row> rows = rs.all();
    assertEquals(1, rows.size());
    assertEquals(VALUE, rows.get(0).getInt("h"));
    assertEquals(VALUE, rows.get(0).getInt("v"));

    revokePermission(SELECT, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    rs = s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedUpdateStmtWithModifyPermission() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantPermission(MODIFY, TABLE, table, username);

    // Prepare and execute statement.
    String updateStmt = String.format("UPDATE %s.%s set v = 1 WHERE h = ?", keyspace, table);
    PreparedStatement stmt = s2.prepare(updateStmt);

    s2.execute(stmt.bind(VALUE));

    revokePermission(MODIFY, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind(VALUE));
  }

  @Test
  public void testPreparedDeleteStmtWithModifyPermission() throws Exception {
    createTableAndInsertRecord(s, keyspace, table);

    grantPermission(MODIFY, TABLE, table, username);

    // Prepare and execute statement.
    String deleteStmt = String.format("DELETE FROM %s.%s WHERE h = ?", keyspace, table);
    PreparedStatement stmt = s2.prepare(deleteStmt);
    s2.execute(stmt.bind(VALUE));

    revokePermission(MODIFY, TABLE, table, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind(VALUE));
  }

  private void testPreparedGrantRevokeRoleStatementWithAuthorizePermission(String stmtType)
      throws Exception {
    String recipientRole = String.format("%s_recipient_%s", username, stmtType);
    String grantedRole = String.format("%s_granted_%s", username, stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    grantPermission(AUTHORIZE, ROLE, grantedRole, username);
    grantPermission(AUTHORIZE, ROLE, recipientRole, username);

    String stmt;
    if (stmtType.equals(GRANT)) {
      stmt = String.format("GRANT %s TO %s", grantedRole, recipientRole);
    } else {
      // Grant the role first using cassandra role.
      s.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));

      stmt = String.format("REVOKE %s FROM %s", grantedRole, recipientRole);
    }
    PreparedStatement preparedStatement = s2.prepare(stmt);
    revokePermission(AUTHORIZE, ROLE, grantedRole, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(preparedStatement.bind());
  }

  private void testPreparedGrantRevokeRoleStatementWithSuperuserRole(String stmtType)
      throws Exception {
    String recipientRole = String.format("recipient_%s_%s_test", username, stmtType);
    String grantedRole = String.format("granted_%s_%s_test", username, stmtType);
    testCreateRoleHelperWithSession(recipientRole, password, false, false, false, s);
    testCreateRoleHelperWithSession(grantedRole, password, false, false, false, s);

    testCreateRoleHelperWithSession(anotherUsername, password, true, true, false, s);
    Session s3 = getSession(anotherUsername, password);

    String stmt;
    if (stmtType.equals(GRANT)) {
      stmt = String.format("GRANT %s TO %s", grantedRole, recipientRole);
    } else {
      // Grant the role first using cassandra role.
      s.execute(String.format("GRANT %s TO %s", grantedRole, recipientRole));

      stmt = String.format("REVOKE %s FROM %s", grantedRole, recipientRole);
    }
    PreparedStatement preparedStatement = s3.prepare(stmt);

    s.execute(String.format("ALTER ROLE %s with SUPERUSER = false", anotherUsername));
    Thread.sleep(TIME_SLEEP_MS);

    thrown.expect(UnauthorizedException.class);
    s3.execute(preparedStatement.bind());
  }

  @Test
  public void testPreparedGrantRoleStatementWithAuthorizePermission() throws Exception {
    testPreparedGrantRevokeRoleStatementWithAuthorizePermission(GRANT);
  }

  @Test
  public void testPreparedRevokeRoleStatementWithAuthorizePermission() throws Exception {
    testPreparedGrantRevokeRoleStatementWithAuthorizePermission(REVOKE);
  }

  @Test
  public void testPreparedGrantRoleStatementWithSuperuserRole() throws Exception {
    testPreparedGrantRevokeRoleStatementWithSuperuserRole(GRANT);
  }

  @Test
  public void testPreparedRevokeRoleStatementWithSuperuserRole() throws Exception {
    testPreparedGrantRevokeRoleStatementWithSuperuserRole(REVOKE);
  }

  @Test
  public void testPreparedGrantPermissionOnKeyspaceWithAuthorizePermission() throws Exception {
    grantPermission(AUTHORIZE, KEYSPACE, keyspace, username);

    String grantPermissionStmt = String.format("GRANT CREATE ON KEYSPACE %s to %s",
        keyspace, username);
    PreparedStatement stmt = s2.prepare(grantPermissionStmt);
    s2.execute(stmt.bind());

    revokePermission(AUTHORIZE, KEYSPACE, keyspace, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedGrantPermissionOnTableWithAuthorizePermission() throws Exception {
    createTableAndVerify(s, keyspace, table);
    grantPermission(AUTHORIZE, TABLE, table, username);

    String grantPermissionStmt = String.format("GRANT MODIFY ON TABLE %s.%s to %s",
        keyspace, table, username);
    PreparedStatement stmt = s2.prepare(grantPermissionStmt);
    s2.execute(stmt.bind());

    revokePermission(AUTHORIZE, TABLE, table, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedGrantPermissionOnRoleStmtWithAuthorizePermission() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, password, false, false, false, s);

    grantPermission(AUTHORIZE, ROLE, anotherUsername, username);

    String grantPermissionStmt = String.format("GRANT DROP ON ROLE %s to %s",
        anotherUsername, username);
    PreparedStatement stmt = s2.prepare(grantPermissionStmt);
    s2.execute(stmt.bind());

    revokePermission(AUTHORIZE, ROLE, anotherUsername, username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedGrantPermissionOnAllKeyspaesWithAuthorizePermission() throws Exception {
    grantPermissionOnAllKeyspaces(AUTHORIZE, username);

    String grantPermissionStmt = String.format("GRANT SELECT ON ALL KEYSPACES TO %s", username);
    PreparedStatement stmt = s2.prepare(grantPermissionStmt);
    s2.execute(stmt.bind());

    revokePermission(AUTHORIZE, ALL_KEYSPACES, "", username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedGrantPermissionOnAllRolesWithAuthorizePermission() throws Exception {
    grantPermissionOnAllRoles(AUTHORIZE, username);

    String grantPermissionStmt = String.format("GRANT DROP ON ALL ROLES TO %s", username);
    PreparedStatement stmt = s2.prepare(grantPermissionStmt);
    s2.execute(stmt.bind());

    revokePermission(AUTHORIZE, ALL_ROLES, "", username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedDropRoleStmtWithDropPermission() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, password, false, false, false, s);
    grantPermission(DROP, ROLE, anotherUsername, username);

    String dropStmt = String.format("DROP ROLE %s", anotherUsername);
    PreparedStatement stmt = s2.prepare(dropStmt);
    s2.execute(stmt.bind());

    // Create it again.
    testCreateRoleHelperWithSession(anotherUsername, password, false, false, false, s);
    revokePermission(DROP, ROLE, anotherUsername, username);
    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedDropKeyspaceStmtWithDropPermission() throws Exception {
    String newKeyspace = "prepared_keyspace";
    createKeyspaceAndVerify(s, newKeyspace);

    // Permission has to be granted on ALL KEYSPACES. Granting DROP permission on a specific
    // keyspace only authorizes the user to drop tables in that keyspace, but not to drop the
    // keyspace.
    grantPermissionOnAllKeyspaces(DROP, username);

    String dropStmt = String.format("DROP KEYSPACE %s", newKeyspace);
    PreparedStatement stmt = s2.prepare(dropStmt);
    s2.execute(stmt.bind());

    revokePermission(DROP, ALL_KEYSPACES, "", username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testPreparedDropTableStmtWithDropPermission() throws Exception {
    createTableAndVerify(s, keyspace, table);

    grantPermission(DROP, TABLE, String.format("%s.%s", keyspace, table), username);

    String dropStmt = String.format("DROP TABLE %s.%s", keyspace, table);
    PreparedStatement stmt = s2.prepare(dropStmt);
    s2.execute(stmt.bind());

    createTableAndVerify(s, keyspace, table);
    revokePermission(DROP, TABLE, String.format("%s.%s", keyspace, table), username);

    thrown.expect(UnauthorizedException.class);
    s2.execute(stmt.bind());
  }

  @Test
  public void testOperationsOnKeyspaceByCreatorRole() throws Exception {
    grantPermissionOnAllKeyspaces(CREATE, username);

    String keyspace2 = keyspace + "_2";

    s2.execute(String.format("CREATE KEYSPACE %s", keyspace2));

    // Revoke CREATE on ALL KEYSPACES to ensure that we are allowed to create tables on keyspace2
    // because we were granted CREATE permission on the new keyspace.
    revokePermission(CREATE, ALL_KEYSPACES, "", username);

    // Create a new table to test the CREATE permission.
    s2.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h))",
        keyspace2, table));

    // Create another table using superuser cassandra. Role 'username' shouldn't have any
    // permissions granted on this table because it's not the creator, but because it has all the
    // permissions granted on the keyspace, 'username' should be able to do any operations on the
    // table.
    String table2 = table + "_2";
    s2.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h))",
        keyspace2, table2));

    // Verify that we can insert a value in table2.
    s2.execute (String.format("INSERT INTO %s.%s (h, v) VALUES (%d, %d)",
        keyspace2, table2, VALUE, VALUE));

    // Verify that we can read a value.
    ResultSet rs = s2.execute (String.format("SELECT * from %s.%s", keyspace2, table2));
    assertEquals(1, rs.all().size());

    // Verify that we can update a value.
    s2.execute(String.format("UPDATE %s.%s SET v = %d WHERE h = %d",
        keyspace2, table2, VALUE + 1, VALUE));

    // Verify that we can delete a value.
    s2.execute(String.format("DELETE FROM %s.%s WHERE h = %d", keyspace2, table2, VALUE));

    // Verify that we can alter the table.
    s2.execute(String.format("ALTER TABLE %s.%s ADD v2 int", keyspace2, table2));

    // Verify that we can drop the table.
    s2.execute(String.format("DROP TABLE %s.%s", keyspace2, table2));

    // Drop the table we created so that we can delete the keyspace (it needs to be empty).
    s2.execute(String.format("DROP TABLE %s.%s", keyspace2, table));

    // Verify that we can delete the keyspace.
    s2.execute(String.format("DROP KEYSPACE %s", keyspace2));
  }

  @Test
  public void testOperationsOnTableByCreatorRole() throws Exception {
    grantPermission(CREATE, KEYSPACE, keyspace, username);

    s2.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h))", keyspace, table));

    // Verify that we can insert a value.
    s2.execute (String.format("INSERT INTO %s.%s (h, v) VALUES (%d, %d)",
        keyspace, table, VALUE, VALUE));

    // Verify that we can read a value.
    ResultSet rs = s2.execute (String.format("SELECT * from %s.%s", keyspace, table));
    assertEquals(1, rs.all().size());

    // Verify that we can update a value.
    s2.execute(String.format("UPDATE %s.%s SET v = %d WHERE h = %d",
        keyspace, table, VALUE + 1, VALUE));

    // Verify that we can delete a value.
    s2.execute(String.format("DELETE FROM %s.%s WHERE h = %d", keyspace, table, VALUE));

    // Verify that we can alter the table.
    s2.execute(String.format("ALTER TABLE %s.%s ADD v2 int", keyspace, table));

    // Verify that we can drop the table.
    s2.execute(String.format("DROP TABLE %s.%s", keyspace, table));
  }

  @Test
  public void testOperationsOnRolesByCreatorRole() throws Exception {
    grantPermissionOnAllRoles(CREATE, username);

    String role1 = username + "_1";
    String role2 = username + "_2";

    // Create two roles.
    s2.execute(String.format("CREATE ROLE %s", role1));
    s2.execute(String.format("CREATE ROLE %s", role2));

    // Alter role1.
    s2.execute(String.format("ALTER ROLE %s WITH LOGIN = TRUE", role1));

    // Grant role1 to role2. It should succeed because we should have AUTHORIZE permission on both
    // roles.
    s2.execute(String.format("GRANT %s to %s", role1, role2));

    // Drop both roles.
    s2.execute(String.format("DROP ROLE %s", role1));
    s2.execute(String.format("DROP ROLE %s", role2));
  }

  @Test
  public void testCreateKeyspaceStmtGrantsPermissionsToCreator() throws Exception {
    // Grant CREATE permission on ALL KEYSPACES so that we can create a new keyspace.
    grantPermissionOnAllKeyspaces(CREATE, username);

    // Crete the keyspace.
    String keyspace2 = keyspace + "_2";
    s2.execute(String.format("CREATE KEYSPACE %s", keyspace2));

    String resource = String.format("data/%s", keyspace2);

    assertPermissionsGranted(s, username, resource, ALL_PERMISSIONS_FOR_KEYSPACE);
  }

  @Test
  public void testCreateTableStmtGrantsPermissionsToCreator() throws Exception {
    // Grant CREATE permission on keyspace so that we can create a new table.
    grantPermission(CREATE, KEYSPACE, keyspace, username);

    // Create the table.
    s2.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h))", keyspace, table));

    List<String> expectedPermissions =
        Arrays.asList("ALTER", "AUTHORIZE", "DROP", "MODIFY", "SELECT");
    String resoure = String.format("data/%s/%s", keyspace, table);

    assertPermissionsGranted(s, username, resoure, expectedPermissions);
  }

  @Test
  public void testCreateRoleStmtGrantsPermissionsToCreator() throws Exception {
    // Grant CREATE permission on ALL ROLES so that we can create a new role.
    grantPermissionOnAllRoles(CREATE, username);

    // Create a new role.
    s2.execute(String.format("CREATE ROLE %s", anotherUsername));

    List<String> expectedPermissions = Arrays.asList("ALTER", "AUTHORIZE", "DROP");
    String resource = String.format("roles/%s", anotherUsername);

    assertPermissionsGranted(s, username, resource, expectedPermissions);
  }

  @Test
  public void testDeletingKeyspaceRemovesPermissionsToo() throws Exception {
    String keyspace2 = keyspace + "_2";
    s.execute(String.format("CREATE KEYSPACE %s", keyspace2));

    grantAllPermission(KEYSPACE, keyspace2, username);
    String resource = String.format("data/%s", keyspace2);
    verifySomePermissionsGranted(username, resource);

    s.execute(String.format("DROP KEYSPACE %s", keyspace2));
    verifyPermissionsDeleted(username, resource);
  }

  @Test
  public void testDeletingTableRemovesPermissionsToo() throws Exception {
    createTableAndVerify(s, keyspace, table);

    grantAllPermission(TABLE, table, username);
    String resource = String.format("data/%s/%s", keyspace, table);
    verifySomePermissionsGranted(username, resource);

    s.execute(String.format("DROP TABLE %s.%s", keyspace, table));
    verifyPermissionsDeleted(username, resource);
  }

  @Test
  public void testDeletingRoleRemovesPermissionsToo() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, password, false, false, false, s);

    grantAllPermission(ROLE, anotherUsername, username);
    String resource = String.format("roles/%s", anotherUsername);
    verifySomePermissionsGranted(username, resource);

    s.execute(String.format("DROP ROLE %s", anotherUsername));
    verifyPermissionsDeleted(username, resource);
  }

  @Test
  public void testNewKeyspaceWithOldNameDoesNotGetOldPermissions() throws Exception {
    String keyspace2 = keyspace + "_2";

    s.execute(String.format("CREATE KEYSPACE %s", keyspace2));

    // Grant all the permissions to username role.
    grantAllPermission(KEYSPACE, keyspace2, username);

    // Create a table and insert a record to verify that username role received the permissions.
    createTableAndInsertRecord(s2, keyspace2, table);

    // Drop the table and keyspace.
    s2.execute(String.format("DROP TABLE %s.%s", keyspace2, table));
    s2.execute(String.format("DROP KEYSPACE %s", keyspace2));

    // Create the keyspace again.
    s.execute(String.format("CREATE KEYSPACE %s", keyspace2));

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    // Verify that username role can't create a table in the new keyspace.
    thrown.expect(UnauthorizedException.class);
    createTableAndVerify(s2, keyspace2, table);
  }

  @Test
  public void testNewTableWithOldNameDoesNotGetOldPermissions() throws Exception {
    createTableAndVerify(s, keyspace, table);

    // Grant all the permissions to username role.
    grantAllPermission(TABLE, table, username);

    // username role should be able to insert a row.
    insertRow(s2, keyspace, table);

    s.execute(String.format("DROP TABLE %s.%s", keyspace, table));

    // Create a new table with the same name.
    createTableAndVerify(s, keyspace, table);

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    // Verify that we can't insert a row again since we haven't granted any permissions to
    // username.
    thrown.expect(UnauthorizedException.class);
    insertRow(s2, keyspace, table);
  }

  @Test
  public void testNewRoleWithOldNameDoesNotGetOldPermissions() throws Exception {
    String role1 = username + "_1";
    String role2 = username + "_2";
    String role3 = username + "_3";

    // Create the roles.
    testCreateRoleHelperWithSession(role1, password, false, false, false, s);
    testCreateRoleHelperWithSession(role2, password, false, false, false, s);
    testCreateRoleHelperWithSession(role3, password, false, false, false, s);

    // Grant all the permissions to username role on the roles we just created.
    grantAllPermission(ROLE, role1, username);
    grantAllPermission(ROLE, role2, username);
    grantAllPermission(ROLE, role3, username);

    verifySomePermissionsGranted(username, "roles/" + role1);
    verifySomePermissionsGranted(username, "roles/" + role2);
    verifySomePermissionsGranted(username, "roles/" + role3);

    // Verify that username role can grant role1 to role2 (AUTHORIZE permissions on both roles
    // needed to do this).
    s2.execute(String.format("GRANT %s to %s", role1, role2));

    // Used to verify that username role has permissions on role2 and role3 roles.
    s2.execute(String.format("GRANT %s to %s", role2, role3));

    // Drop role1 role.
    s2.execute(String.format("DROP ROLE %s", role1));

    // Create role1 role again.
    testCreateRoleHelperWithSession(role1, password, false, false, false, s);

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    // Verify that we can't grant role1 to role3 since username role shouldn't have any permissions
    // on role1 role.
    thrown.expect(UnauthorizedException.class);
    s2.execute(String.format("GRANT %s to %s", role1, role3));
  }

  // Test that we can grant and revoke permissions on a table without using the keyword TABLE before
  // the table name.
  @Test
  public void testGrantPermissionOnTableWithoutUsingKeywordTable() throws Exception {
    createTableAndVerify(s, keyspace, table);
    s.execute(String.format("GRANT MODIFY ON %s.%s TO %s", keyspace, table, username));
    String canonicalResource = String.format("data/%s/%s", keyspace, table);
    assertPermissionsGranted(s, username, canonicalResource, Arrays.asList(MODIFY));
  }

  @Test
  public void testRevokePermissionOnTableWithoutUsingKeywordTable() throws Exception {
    createTableAndVerify(s, keyspace, table);
    String canonicalResource = String.format("data/%s/%s", keyspace, table);

    grantPermission(SELECT, TABLE, String.format("%s.%s", keyspace, table), username);
    assertPermissionsGranted(s, username, canonicalResource, Arrays.asList(SELECT));

    s.execute(String.format("REVOKE SELECT ON %s.%s FROM %s", keyspace, table, username));
    assertPermissionsGranted(s, username, canonicalResource, Arrays.asList());
  }

  // This tests the fix for issue https://github.com/YugaByte/yugabyte-db/issues/592.
  @Test
  public void testAlterStmtFailsWihoutProperties() throws Exception {
    thrown.expect(SyntaxError.class);
    thrown.expectMessage("expecting WITH");
    s.execute(String.format("ALTER ROLE %s", username));
  }

  @Test
  public void testAlterModifiesProperties() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, "", false, false, false, s);
    String newPassword = "p";
    s.execute(String.format(
        "ALTER ROLE %s WITH LOGIN = true AND SUPERUSER = true AND PASSWORD = '%s'",
        anotherUsername, newPassword));
    ResultSet rs = s.execute(String.format("SELECT * FROM system_auth.roles WHERE role = '%s'",
        anotherUsername));
    List<Row> list = rs.all();
    assertEquals(1, list.size());
    assert(list.get(0).getBool("can_login"));
    assert(list.get(0).getBool("is_superuser"));
    checkConnectivity(true, anotherUsername, newPassword, false);
  }


  // Test for https://github.com/yugabyte/yugabyte-db/issues/2505.
  @Test
  public void testAlterOwnSuperuserStatusFails() throws Exception {
    thrown.expect(UnauthorizedException.class);
    thrown.expectMessage("Unauthorized. You aren't allowed to alter your own superuser status or " +
            "that of a role granted to you");
    s.execute("ALTER ROLE cassandra WITH SUPERUSER = false");
  }

  // Test for https://github.com/yugabyte/yugabyte-db/issues/2505.
  @Test
  public void testAlterSuperuserStatusOfGrantedRoleFails() throws Exception {
    testCreateRoleHelperWithSession("parent", "", false, true, false, s);
    testCreateRoleHelperWithSession("grandparent", "", false, true, false, s);

    s.execute("GRANT grandparent TO parent");
    s.execute("GRANT parent TO cassandra");

    thrown.expect(UnauthorizedException.class);
    thrown.expectMessage("Unauthorized. You aren't allowed to alter your own superuser status or " +
            "that of a role granted to you");
    s.execute("ALTER ROLE grandparent WITH SUPERUSER = false");
  }

  @Test
  public void testNotEmptyResourcesInSytemAuthRolePermissionsTable() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, "", false, false, false, s);

    String canonicalResource = String.format("roles/%s", anotherUsername);
    List<String> expectedPermissions = Arrays.asList(ALTER, AUTHORIZE, DROP);
    // Test that we can see the permissions when we query system_auth.role_permissions.
    assertPermissionsGranted(s, "cassandra", canonicalResource, expectedPermissions);

    for (String permission : expectedPermissions) {
      revokePermissionNoSleep(permission, ROLE, anotherUsername, "cassandra");
    }

    // Verify the resource doesn't appear anymore.
    String stmt = String.format("SELECT permissions FROM system_auth.role_permissions " +
        "WHERE role = 'cassandra' and resource = '%s';", canonicalResource);
    List<Row> rows = s.execute(stmt).all();
    assert(rows.isEmpty());
  }

  @Test
  public void testInheritedPermissions() throws Exception {
    String level0 = "level0";
    String level1 = "level1";
    String level2 = "level2";
    String level3_0 = "level3_0";
    String level3_1 = "level3_1";

    testCreateRoleHelperWithSession(level0, password, /* canLogin */ true,
            /* isSuperuser */false, /*verifyConnectivity */ false, /* session */ s);
    testCreateRoleHelperWithSession(level1, "", false, false, false, s);
    testCreateRoleHelperWithSession(level2, "", false, false, false, s);
    testCreateRoleHelperWithSession(level3_0, "", false, false, false, s);
    testCreateRoleHelperWithSession(level3_1, "", false, false, false, s);

    s.execute(String.format("GRANT %s TO %s", level3_0, level2));
    s.execute(String.format("GRANT %s TO %s", level3_1, level2));
    s.execute(String.format("GRANT %s TO %s", level2, level1));
    s.execute(String.format("GRANT %s TO %s", level1, level0));

    s.execute(String.format("GRANT CREATE ON ALL KEYSPACES TO %s", level3_0));

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    // Verify that level0 can create a keyspace since it has inherited that permissions from
    // level3_0.
    Session level0Session = getSession(level0, password);
    level0Session.execute("CREATE KEYSPACE somekeyspace");


    // Grant CREATE ON ALL ROLES to level3_1 and verify that level0 role can create a role.
    s.execute(String.format("GRANT CREATE ON ALL ROLES TO %s", level3_1));

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    level0Session.execute("CREATE ROLE somerole");

    s.execute(String.format("GRANT DROP ON ALL KEYSPACES TO %s", level3_1));

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    // Verify again that level0 can create a keyspace since it is now inheriting two different
    // permissions on ALL KEYSPACES from two different roles.
    level0Session.execute("CREATE KEYSPACE somekeyspace2");
  }

  // This test a fix for https://github.com/yugabyte/yugabyte-db/issues/4062.
  // The issue is that when different permissions are inherited from roles granted to another role,
  // it's possible that they might replace the permissions granted directly.
  // For example:
  // If role 'eng' has permission SELECT on table 'releases', and role 'john' has permission
  // MODIFY on the same table, and role 'eng' is granted to 'john', it's possible (depending on
  // the order the permissions are received) that permission SELECT will replace permission
  // MODIFY on table 'releases'.
  @Test
  public void testInheritedPermissionsDoNotOverrideGrantedPermissions() throws Exception {
    createTableAndVerify(s, keyspace, table);

    testCreateRoleHelperWithSession("employees", password, /* canLogin */ true,
        /* isSuperuser */false, /*verifyConnectivity */ false, /* session */ s);
    testCreateRoleHelperWithSession("eng", password, /* canLogin */ true,
        /* isSuperuser */false, /*verifyConnectivity */ false, /* session */ s);

    s.execute(String.format("GRANT employees TO eng"));
    s.execute(String.format("GRANT SELECT ON %s.%s TO eng", keyspace, table));

    testCreateRoleHelperWithSession("john", password, true, false, false, s);

    s.execute(String.format("GRANT eng TO john"));
    s.execute(String.format("GRANT MODIFY ON %s.%s TO john", keyspace, table));


    Session johnSession = getSession("john", password);

    // Sleep to give the cache some time to be refreshed.
    Thread.sleep(TIME_SLEEP_MS);

    // Verify that user 'john' can insert a record in the table.
    insertRow(johnSession, keyspace, table);
    johnSession.execute(String.format("SELECT * FROM %s.%s", keyspace, table));
  }

  public void testGrantAllGrantsCorrectPermissions() throws Exception {
    createTableAndVerify(s, keyspace, table);
    testCreateRoleHelperWithSession(anotherUsername, "a", false, false, false, s);

    grantAllPermission(KEYSPACE, keyspace, username);
    assertPermissionsGranted(s, username, "data/" + keyspace,
        Arrays.asList(ALTER, AUTHORIZE, CREATE, DROP, MODIFY, SELECT));

    grantAllPermission(TABLE, String.format("%s.%s", keyspace, table), username);
    assertPermissionsGranted(s, username, String.format("data/%s/%s", keyspace, table),
        Arrays.asList(ALTER, AUTHORIZE, DROP, MODIFY, SELECT));

    grantAllPermission(ROLE, anotherUsername, username);
    assertPermissionsGranted(s, username, "roles/" + anotherUsername,
        Arrays.asList(ALTER, AUTHORIZE, DROP));

    grantPermissionOnAllKeyspaces(ALL, username);
    assertPermissionsGranted(s, username, "data",
        Arrays.asList(ALTER, AUTHORIZE, CREATE, DROP, MODIFY, SELECT));

    grantPermissionOnAllRoles(ALL, username);
    grantAllPermission(ROLE, anotherUsername, username);
    assertPermissionsGranted(s, username, "roles",
        Arrays.asList(ALTER, AUTHORIZE, CREATE, DESCRIBE, DROP));
  }

  private void testPermissionOnResourceFails(String permission, String resourceType,
      String resourceName, String receivingRole) throws Exception {
    thrown.expect(com.datastax.driver.core.exceptions.SyntaxError.class);
    thrown.expectMessage(
        "Resource type DataResource does not support any of the requested permissions");
    grantPermission(permission, resourceType, resourceName, receivingRole);
  }

  @Test
  public void testGrantDescribeOnKeyspaceFails() throws Exception {
    testPermissionOnResourceFails(DESCRIBE, KEYSPACE, keyspace, username);
  }

  @Test
  public void testGrantDescribeOnAllKeyspacesFails() throws Exception {
    testPermissionOnResourceFails(DESCRIBE, ALL_KEYSPACES, "", username);
  }

  @Test
  public void testGrantDescribeOnTableFails() throws Exception {
    createTableAndVerify(s, keyspace, table);
    testPermissionOnResourceFails(DESCRIBE, TABLE, String.format("%s.%s", keyspace, table),
        username);
  }

  @Test
  public void testGrantDescribeOnRoleFails() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, "a", false, false, false, s);
    testPermissionOnResourceFails(DESCRIBE, ROLE, anotherUsername, username);
  }

  @Test
  public void testGrantCreateOnTableFails() throws Exception {
    createTableAndVerify(s, keyspace, table);
    testPermissionOnResourceFails(CREATE, TABLE, String.format("%s.%s", keyspace, table), username);
  }

  @Test
  public void testGrantCreateOnRoleFails() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, "a", false, false, false, s);
    testPermissionOnResourceFails(CREATE, ROLE, username, anotherUsername);
  }

  @Test
  public void testGrantModifyOnRoleFails() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, "a", false, false, false, s);
    testPermissionOnResourceFails(MODIFY, ROLE, username, anotherUsername);
  }

  @Test
  public void testGrantSelectOnRoleFails() throws Exception {
    testCreateRoleHelperWithSession(anotherUsername, "a", false, false, false, s);
    testPermissionOnResourceFails(SELECT, ROLE, username, anotherUsername);
  }

  @Test
  public void testGrantModifyOnAllRoleFails() throws Exception {
    testPermissionOnResourceFails(MODIFY, ALL_ROLES, "", username);
  }

  @Test
  public void testGrantSelectOnAllRoleFails() throws Exception {
    testPermissionOnResourceFails(SELECT, ALL_ROLES, "", username);
  }

  @Test
  public void testCreateIndexWithCreateTablePermission() throws Exception {
    s.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h)) " +
        "WITH transactions = { 'enabled' : true }", keyspace, table));
    s.execute("USE " + keyspace);

    grantPermission(CREATE, KEYSPACE, keyspace, username);

    String createIndexStmt = String.format("CREATE INDEX order_by_v on %s.%s (v)",
        keyspace, table);

    thrown.expect(UnauthorizedException.class);
    s2.execute(createIndexStmt);
  }

  @Test
  public void testCreateIndexWithAlterTablePermission() throws Exception {
    s.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h)) " +
        "WITH transactions = { 'enabled' : true }", keyspace, table));
    s.execute("USE " + keyspace);
    grantPermission(ALTER, TABLE, table, username);

    String createIndexStmt = String.format("CREATE INDEX order_by_v on %s.%s (v)",
        keyspace, table);

    s2.execute(createIndexStmt);
  }

  @Test
  public void testDropIndexWithWrongTablePermission() throws Exception {
    s.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h)) " +
        "WITH transactions = { 'enabled' : true }", keyspace, table));
    s.execute("USE " + keyspace);
    grantPermission(DROP, TABLE, table, username);

    String indexName = "drop_test_order_by_v_2";

    String createIndexStmt = String.format("CREATE INDEX %s on %s.%s (v)",
        indexName, keyspace, table);

    s.execute(createIndexStmt);

    Thread.sleep(1000);

    String dropIndexStmt = String.format("DROP INDEX %s.%s", keyspace, indexName);
    thrown.expect(UnauthorizedException.class);
    thrown.expectMessage(String.format(
        "User %s has no ALTER permission on <table %s.%s> or any of its parents",
        username, keyspace, table));
    s2.execute(dropIndexStmt);
  }

  @Test
  public void testDropIndexWithAlterTablePermission() throws Exception {
    s.execute(String.format("CREATE TABLE %s.%s (h int, v int, PRIMARY KEY(h)) " +
        "WITH transactions = { 'enabled' : true }", keyspace, table));
    s.execute("USE " + keyspace);
    grantPermission(ALTER, TABLE, table, username);

    String indexName = "drop_test_order_by_v_3";

    String createIndexStmt = String.format("CREATE INDEX %s on %s.%s (v)",
        indexName, keyspace, table);

    s.execute(createIndexStmt);

    String dropIndexStmt = String.format("DROP INDEX %s.%s", keyspace, indexName);
    s2.execute(dropIndexStmt);
  }

  @Test
  public void testDropTypeWithAllKeyspacesPermission() throws Exception {
    String typeName = "test_type";
    String typeName2 = typeName + "_2";

    LOG.info("Begin test");
    grantPermissionOnAllKeyspaces(CREATE, username);

    s2.execute(String.format("CREATE KEYSPACE %s", anotherKeyspace));
    s2.execute(String.format("CREATE TYPE %s.%s (id TEXT)", anotherKeyspace, typeName));
    // Type owner must be able to drop own type.
    s2.execute(String.format("DROP TYPE %s.%s", anotherKeyspace, typeName));

    s2.execute("USE " + anotherKeyspace);
    s2.execute(String.format("CREATE TYPE %s (id INT)", typeName2));
    s2.execute(String.format("DROP TYPE %s", typeName2));

    LOG.info("End test");
  }

  @Test
  public void testCreateTypeWithKeyspacePermission() throws Exception {
    String typeName = "test_type";
    String typeName2 = typeName + "_2";

    LOG.info("Begin test");
    s.execute(String.format("CREATE KEYSPACE %s", anotherKeyspace));

    try {
      s2.execute(String.format("CREATE TYPE %s.%s (id TEXT)", anotherKeyspace, typeName));
      fail("CREATE TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }
    s2.execute("USE " + anotherKeyspace);
    try {
      s2.execute(String.format("CREATE TYPE %s (id INT)", typeName2));
      fail("CREATE TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(CREATE, KEYSPACE, anotherKeyspace, username);
    s2.execute(String.format("CREATE TYPE %s.%s (id TEXT)", anotherKeyspace, typeName));
    s2.execute(String.format("CREATE TYPE %s (id INT)", typeName2));

    try {
      s2.execute(String.format("DROP TYPE %s.%s", anotherKeyspace, typeName));
      fail("DROP TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }
    try {
      s2.execute(String.format("DROP TYPE %s", typeName2));
      fail("DROP TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(DROP, KEYSPACE, anotherKeyspace, username);
    s2.execute(String.format("DROP TYPE %s.%s", anotherKeyspace, typeName));
    s2.execute(String.format("DROP TYPE %s", typeName2));

    LOG.info("End test");
  }

  private void internalTestPreparedCreateDropType(String resourceType,
                                                  String resource) throws Exception {
    String typeName = "test_type";
    String typeName2 = typeName + "_2";
    s.execute(String.format("CREATE KEYSPACE %s", anotherKeyspace));

    LOG.info("Begin test");

    // Prepare and execute statements.
    String createTypeStmt = String.format("CREATE TYPE %s.%s (id TEXT)", anotherKeyspace, typeName);
    grantPermission(CREATE, resourceType, resource, username);
    PreparedStatement prepCreateStmt = s2.prepare(createTypeStmt);
    revokePermission(CREATE, resourceType, resource, username);
    try {
      s2.execute(prepCreateStmt.bind());
      fail("Prepared CREATE TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(CREATE, resourceType, resource, username);
    s2.execute(prepCreateStmt.bind());

    String dropTypeStmt = String.format("DROP TYPE %s.%s", anotherKeyspace, typeName);
    grantPermission(DROP, resourceType, resource, username);
    PreparedStatement prepDropStmt = s2.prepare(dropTypeStmt);
    revokePermission(DROP, resourceType, resource, username);
    try {
      s2.execute(prepDropStmt.bind());
      fail("Prepared DROP TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(DROP, resourceType, resource, username);
    s2.execute(prepDropStmt.bind());

    s2.execute("USE " + anotherKeyspace);

    // Prepare and execute statements.
    String createTypeStmt2 = String.format("CREATE TYPE %s (id TEXT)", typeName2);
    PreparedStatement prepCreateStmt2 = s2.prepare(createTypeStmt2);
    revokePermission(CREATE, resourceType, resource, username);
    try {
      s2.execute(prepCreateStmt2.bind());
      fail("Prepared CREATE TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(CREATE, resourceType, resource, username);
    s2.execute(prepCreateStmt2.bind());

    String dropTypeStmt2 = String.format("DROP TYPE %s", typeName2);
    PreparedStatement prepDropStmt2 = s2.prepare(dropTypeStmt2);
    revokePermission(DROP, resourceType, resource, username);
    try {
      s2.execute(prepDropStmt2.bind());
      fail("Prepared DROP TYPE works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(DROP, resourceType, resource, username);
    s2.execute(prepDropStmt2.bind());

    LOG.info("End test");
  }

  @Test
  public void testPreparedCreateDropTypeWithAllKeyspacesPermission() throws Exception {
    // Test prepared statements with ALL_KEYSPACES permissions.
    internalTestPreparedCreateDropType(ALL_KEYSPACES, "");
  }

  @Test
  public void testPreparedCreateDropTypeWithKeyspacePermission() throws Exception {
    // Test prepared statements with used keyspace permissions.
    internalTestPreparedCreateDropType(KEYSPACE, anotherKeyspace);
  }

  @Test
  public void testExplain() throws Exception {
    LOG.info("Begin test");

    s.execute(String.format("CREATE TABLE %s.%s (auth_id text, PRIMARY KEY(auth_id))",
                            keyspace, table));
    String explainStmt = String.format("EXPLAIN SELECT * FROM %s.%s WHERE auth_id=''",
                                       keyspace, table);
    try {
      s2.execute(explainStmt);
      fail("EXPLAIN SELECT works without permissions");
    } catch (com.datastax.driver.core.exceptions.UnauthorizedException e) {
      LOG.info("Expected exception:", e);
    }

    grantPermission(SELECT, TABLE, keyspace + '.' + table, username);
    s2.execute(explainStmt);

    LOG.info("End test");
  }
}
