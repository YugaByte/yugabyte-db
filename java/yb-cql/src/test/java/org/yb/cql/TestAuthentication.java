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

import com.datastax.driver.core.*;
import com.datastax.driver.core.ProtocolOptions.Compression;

import com.datastax.driver.core.exceptions.UnauthorizedException;
import org.junit.Test;

import static org.yb.AssertionWrappers.*;

import org.yb.YBTestRunner;
import org.yb.minicluster.BaseMiniClusterTest;

import org.junit.BeforeClass;
import org.junit.runner.RunWith;

@RunWith(value=YBTestRunner.class)
public class TestAuthentication extends BaseAuthenticationCommonCQLTest {

  @Test(timeout = 100000)
  public void testCreateRoles() throws Exception {
    testCreateRoleHelper("role1", "$!@$q1<>?", false, false);
    testCreateRoleHelper("role4", "$!@$q1<>?", false, true);
    testCreateRoleHelper("role2", "$!@$q1<>?", true, false);
    testCreateRoleHelper("role3", "$!@$q1<>?", true, true);
  }

  @Test(timeout = 100000)
  public void testDeleteNonExistingRole() throws Exception {
    Session s = getDefaultSession();

    // Delete a role that doesn't exist.
    String deleteStmt = "DROP ROLE other_role";
    thrown.expect(com.datastax.driver.core.exceptions.InvalidQueryException.class);
    thrown.expectMessage("Role other_role does not exist");
    s.execute(deleteStmt);
  }

  @Test
  public void testNoSuperuserCannotDeleteSuperuserRole() throws Exception {
    String superuser = "superuser1";
    String user = "no_superuser";
    String pwd = "password";

    // For now we create this user as a superuser. We will change that attribute later.
    testCreateRoleHelper(user, pwd, true, true);

    Session s2 = getSession(user, pwd);

    // Create another superuser role using user's session so that it gets all the permissions on
    // the new role.
    testCreateRoleHelperWithSession(superuser, pwd, true, true, false, s2);

    Session s = getDefaultSession();
    s.execute(String.format("ALTER ROLE %s WITH SUPERUSER = false", user));

    // A non-superuser role tries to delete a superuser role.
    thrown.expect(com.datastax.driver.core.exceptions.UnauthorizedException.class);
    thrown.expectMessage("Only superusers can drop a role with superuser status");
    s2.execute(String.format("DROP ROLE %s", superuser));
  }

  @Test(timeout = 100000)
  public void testAlterSuperuserForExistingRole() throws Exception {
    Session s = getDefaultSession();

    String roleName = "alter_test_3";
    String password = "!%^()(*~`";
    testCreateRoleHelper(roleName, password, true, false);

    // Verify that we can login.
    checkConnectivity(true, roleName, password, false);

    // Make the role a super user.
    String alterStmt = String.format("ALTER ROLE %s with SUPERUSER = true", roleName);
    s.execute(alterStmt);

    // During the first iteration we check tha the changes were applied correctly to the
    // in-memory structures. Then we restart the cluster to verify that the changes were saved
    // to disk and are loaded correctly.
    for (int i = 0; i < 2; i++) {
      verifyRole(roleName, true, true);

      // Verify that we can still login. Making a role a super user shouldn't affect connectivity.
      checkConnectivity(true, roleName, password, false);

      if (i == 0) {
        miniCluster.restart();
      }
    }
  }

  @Test(timeout = 100000)
  public void testAlterNonExistingRole() throws Exception {
    Session s = getDefaultSession();
    String alterStmt = "ALTER ROLE alter_test_5 WITH LOGIN = false";

    thrown.expect(com.datastax.driver.core.exceptions.InvalidQueryException.class);
    thrown.expectMessage("Role alter_test_5 does not exist");
    s.execute(alterStmt);
  }

  @Test(timeout = 100000)
  public void testConnectWithDefaultUserPass() throws Exception {
    checkConnectivity(true, "cassandra", "cassandra", false);
  }

  @Test(timeout = 100000)
  public void testConnectWithWrongPass() throws Exception {
    testCreateRoleHelper("someuser", "somepass", true, true);
    checkConnectivity(true, "someuser", "wrongpass", true);
  }

  @Test(timeout = 100000)
  public void testConnectWithNoLogin() throws Exception {
    String createStmt = String.format("CREATE ROLE 'usernnologin' WITH PASSWORD='abc'");
    Session s = getDefaultSession();
    s.execute(createStmt);
    checkConnectivityWithMessage(true, "usernnologin", "abc",
        ProtocolOptions.Compression.NONE,true, "is not permitted to log in");
  }

  @Test(timeout = 100000)
  public void testConnectWihtNoLoginAndNoPass() throws Exception {
    String createStmt = String.format("CREATE ROLE 'usernnologinnopass'");
    Session s = getDefaultSession();
    s.execute(createStmt);
    checkConnectivityWithMessage(true, "usernnologinnopass", "abc",
        ProtocolOptions.Compression.NONE,true, "and/or password are incorrect");
  }

  // This tests fix for https://github.com/yugabyte/yugabyte-db/issues/4459.
  // Before this fix, the tserver process would crash when trying to check the credentials because
  // it couldn't handle roles that were created without a password.
  @Test(timeout = 100000)
    public void testConnectWithRoleThatHasNoPass() throws Exception {
    String createStmt = String.format("CREATE ROLE 'usernopass' WITH LOGIN = true");
    Session s = getDefaultSession();
    s.execute(createStmt);
    checkConnectivity(true, "usernopass", "abc", true);
  }

  @Test(timeout = 100000)
  public void testConnectWithFakeUserPass() throws Exception {
    checkConnectivity(true, "fakeUser", "fakePass", true);
  }

  @Test(timeout = 100000)
  public void testConnectNoUserPass() throws Exception {
    checkConnectivity(false, null, null, true);
  }

  @Test(timeout = 100000)
  public void testConnectWithDefaultUserPassAndCompression() throws Exception {
    checkConnectivity(true, "cassandra", "cassandra", Compression.LZ4, false);
    checkConnectivity(true, "cassandra", "cassandra", Compression.SNAPPY, false);
    checkConnectivity(true, "fakeUser", "fakePass", Compression.LZ4, true);
    checkConnectivity(true, "fakeUser", "fakePass", Compression.SNAPPY, true);
    checkConnectivity(false, null, null, Compression.LZ4, true);
    checkConnectivity(false, null, null, Compression.SNAPPY, true);
  }

  @Test
  public void testNonSuperuserRoleCannotCreateSuperuserRole() throws Exception {
    Session s = getDefaultSession();
    String roleName = "non_superuser";
    String password = "abc";
    testCreateRoleHelperWithSession(roleName, password, true, false, false, s);

    grantPermission(CREATE, ALL_ROLES, "", roleName, s);

    Session s2 = getSession(roleName, password);
    // Verify that we can create a simple role.
    s2.execute("CREATE ROLE simple_role");

    // Verify that we can't create a superuser role.
    thrown.expect(UnauthorizedException.class);
    s2.execute("CREATE ROLE some_superuser_role WITH SUPERUSER = true");
  }

  @Test
  public void testNonSuperuserRoleCannotCreateIfNotExistsSuperuserRole() throws Exception {
    Session s = getDefaultSession();
    String roleName = "non_superuser_2";
    String password = "abc";
    testCreateRoleHelperWithSession(roleName, password, true, false, false, s);

    grantPermission(CREATE, ALL_ROLES, "", roleName, s);

    Session s2 = getSession(roleName, password);
    // Verify that we can create a simple role.
    s2.execute("CREATE ROLE simple_role_2");

    // Verify that CREATE IF NOT EXISTS for an existing role, fails with an unauthorized exception.
    thrown.expect(UnauthorizedException.class);
    s2.execute("CREATE ROLE IF NOT EXISTS simple_role_2 WITH SUPERUSER = true");
  }

  @Test
  public void testSuperuserRoleCanCreateSuperuserRole() throws Exception {
    Session s = getDefaultSession();
    String roleName = "superuser_role";
    String password = "abc";
    testCreateRoleHelperWithSession(roleName, password, true, true, false, s);

    // No need to grant permissions to the superuser role.
    Session s2 = getSession(roleName, password);
    s2.execute("CREATE ROLE some_superuser_role WITH SUPERUSER = true");
  }

  @Test
  public void testAlterSuperuserFieldOfSuperuserRole() throws Exception {
    String password = "abc";
    String superuser = "alter_superuser_test_superuser";
    String nonSuperuser = "alter_superuser_test_non_superuser";

    // Initially create this user with the superuser status, so that it can create another
    // another superuser. We will remove this role' superuser status before trying to alter a
    // superuser role.
    testCreateRoleHelper(nonSuperuser, password, true, true);

    Session s2 = getSession(nonSuperuser, password);

    // Using nonSuperuser role to create superuser so that it automatically acquires all the
    // permissions on superuser.
    testCreateRoleHelperWithSession(superuser, password, true, true, true, s2);

    // Remove the superuser status from nonSuperuser to trigger the failure.
    getDefaultSession().execute(String.format("ALTER ROLE %s WITH SUPERUSER = FALSE",
                                              nonSuperuser));

    // A non-superuser role tries to alter the superuser field of a superuser role.
    String alterStmt = String.format("ALTER ROLE %s WITH SUPERUSER = FALSE", superuser);
    thrown.expect(com.datastax.driver.core.exceptions.UnauthorizedException.class);
    thrown.expectMessage("Only superusers are allowed to alter superuser status");
    s2.execute(alterStmt);
  }

  // The previous test verifies that a non-superuser role cannot alter the superuser status of a
  // superuser role. This test verifies that a non-superuser role cannot modify the superuser status
  // of a non-superuser role.
  @Test
  public void testAlterSuperuserFieldOfNonSuperuserRole() throws Exception {
    String password = "abc";
    String nonSuperuser = "nosuperuser_role_123";
    String anotherNonSuperuser = "another_nosuperuser_role_123";

    testCreateRoleHelper(nonSuperuser, password, /* canLogin */ true, /* isSuperuser */ false);
    getDefaultSession().execute(String.format("GRANT ALL ON ALL ROLES to %s", nonSuperuser));
    getDefaultSession().execute(String.format("GRANT ALL ON ALL KEYSPACES to %s", nonSuperuser));

    Session s2 = getSession(nonSuperuser, password);

    // Using nonSuperuser role to create anotherNonSuperuser so that it automatically acquires all
    // the permissions on anotherNonSuperuser.
    testCreateRoleHelperWithSession(anotherNonSuperuser, password, /* canLogin */ true,
            /* isSuperuser */false, /* verifyConnectivity */ false, /* Session */ s2);

    // Try to modify the superuser status from anotherNonSuperuser to trigger the failure.
    String alterStmt = String.format("ALTER ROLE %s WITH SUPERUSER = FALSE", anotherNonSuperuser);
    thrown.expect(com.datastax.driver.core.exceptions.UnauthorizedException.class);
    thrown.expectMessage("Only superusers are allowed to alter superuser status");
    s2.execute(alterStmt);
  }

  @Test
  public void testRoleCannotDeleteItself() throws Exception {
    String user = "delete_itself_role";
    String password = "abc";

    testCreateRoleHelper(user, password, true, true);

    Session s2 = getSession(user, password);
    thrown.expect(com.datastax.driver.core.exceptions.InvalidQueryException.class);
    thrown.expectMessage("Cannot DROP primary role for current login");
    s2.execute(String.format("DROP ROLE %s", user));
  }

  @Test
  public void testDuplicatePasswordIsValid() throws Exception {
    String role1 = "duplicate_password_role1";
    String role2 = "duplicate_password_role2";
    String password = "abc";

    testCreateRoleHelper(role1, password, true, false);
    testCreateRoleHelper(role2, password, true, false);

    Session s1 = getSession(role1, password);
    Session s2 = getSession(role2, password);
  }

  @Test
  public void testMultipleLoginWithinCache() throws Exception {
    int roleCount = 4;
    String password = "abc";

    for (int i = 0; i < roleCount; i++) {
      String role = String.format("cache_role%d", i);
      testCreateRoleHelper(role, password, true, false);
    }

    for (int j = 0; j < 3; j++) {
      for (int i = 0; i < roleCount; i++) {
        String role = String.format("cache_role%d", i);
        Session s = getSession(role, password);
      }
    }
  }

  @Test(timeout=500000)
  public void testLoginExhaustCache() throws Exception {
    int roleCount = 10;
    String password = "abc";

    for (int i = 0; i < roleCount; i++) {
      String role = String.format("exhaust_role%d", i);
      testCreateRoleHelper(role, password, true, false);
    }

    for (int i = 0; i < roleCount; i++) {
      String role = String.format("exhaust_role%d", i);
      Session s = getSession(role, password);
    }
  }
}
