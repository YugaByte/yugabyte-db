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

import org.junit.runner.RunWith;

import java.util.Map;

abstract public class BaseAuthenticationCommonCQLTest extends BaseAuthenticationCQLTest {
  int getPermissionUpdateInterval() {
    return 2000;
  }

  int getSleepInterval() {
    // Wait duration after role change so that permission cache refreshes with latest permission
    // derived from value of update_permissions_cache_msecs gflag, adding 100 ms for good measure
    return getPermissionUpdateInterval() + 100;
  }

  protected Map<String, String> getTServerFlags() {
    Map<String, String> flagMap = super.getTServerFlags();
    flagMap.put("update_permissions_cache_msecs", Integer.toString(getSleepInterval()));
    return flagMap;
  }

  @Test(timeout = 100000)
  public void testDeleteExistingRole() throws Exception {
    // Create the role.
    String roleName = "role_test";
    String password = "adsfQ%T!qaewfa";
    testCreateRoleHelper(roleName, password, true, false);

    Session s = getDefaultSession();
    // Delete the role.
    String deleteStmt = String.format("DROP ROLE %s", roleName);
    s.execute(deleteStmt);
    Thread.sleep(getSleepInterval());

    // Verify that we cannot connect using the deleted role.
    checkConnectivity(true, roleName, password, true);
  }

  @Test
  public void testSuperuserCanDeleteAnotherSuperuserRole() throws Exception {
    String superuser1 = "superuser_1";
    String superuser2 = "superuser_2";
    String pwd = "password";

    testCreateRoleHelper(superuser1, pwd, true, true);

    Session s2 = getSession(superuser1, pwd);

    // Create a new role using superuser1's session. This should grant all the permissions on role
    // superuser2 to superuser1.
    testCreateRoleHelperWithSession(superuser2, pwd, true, true, true, s2);

    // Verify that superuser1 can delete superuser2.
    s2.execute(String.format("DROP ROLE %s", superuser2));
    Thread.sleep(getSleepInterval());

    // Verify that we can't connect using the deleted role.
    checkConnectivity(true, superuser2, pwd, true);
  }

  @Test(timeout = 100000)
  public void testAlterPasswordForExistingRole() throws Exception {
    Session s = getDefaultSession();

    String roleName = "alter_test_1";
    String oldPassword = "!%^()(*~`";
    testCreateRoleHelper(roleName, oldPassword, true, false);

    // Change the role's password.
    String newPassword = "%#^$%@$@";
    String alterStmt = String.format("ALTER ROLE %s with PASSWORD = '%s'", roleName, newPassword);
    s.execute(alterStmt);
    Thread.sleep(getSleepInterval());

    // During the first iteration we check tha the changes were applied correctly to the
    // in-memory structures. Then we restart the cluster to verify that the changes were saved
    // to disk and are loaded correctly.
    for (int i = 0; i < 2; i++) {
      verifyRole(roleName, true, false);

      // Verify that we cannot connect using the old password.
      checkConnectivity(true, roleName, oldPassword, true);

      // Verify that we can connect using the new password.
      checkConnectivity(true, roleName, newPassword, false);

      if (i == 0) {
        miniCluster.restart();
      }
    }

  }

  @Test(timeout = 100000)
  public void testAlterLoginForExistingRole() throws Exception {
    Session s = getDefaultSession();

    String roleName = "alter_test_2";
    String password = "!%^()(*~`";
    testCreateRoleHelper(roleName, password, true, false);

    // Verify that we can login.
    checkConnectivity(true, roleName, password, false);

    // Change the role LOGIN from true to false.
    String alterStmt = String.format("ALTER ROLE %s with LOGIN = false", roleName);
    s.execute(alterStmt);
    Thread.sleep(getSleepInterval());

    // During the first iteration we check tha the changes were applied correctly to the
    // in-memory structures. Then we restart the cluster to verify that the changes were saved
    // to disk and are loaded correctly.
    for (int i = 0; i < 2; i++) {

      verifyRole(roleName, false, false);

      // Verify that we cannot longer login.
      checkConnectivity(true, roleName, password, true);

      if (i == 0) {
        miniCluster.restart();
      }
    }
  }

  @Test(timeout = 100000)
  public void testAlterPasswordAndLoginAndSuperuserExistingRole() throws Exception {
    Session s = getDefaultSession();

    String roleName = "alter_test_4";
    String oldPassword = "sdf$hgfaY13";
    testCreateRoleHelper(roleName, oldPassword, false, false);

    // Verify that we cannot login because the role was created with LOGIN = false.
    checkConnectivity(true, roleName, oldPassword, true);

    // Change the role's password, LOGIN from false to true, and SUPERUSER from false to true.
    String newPassword = "*&oi2jr8OI";
    String alterStmt = String.format(
        "ALTER ROLE %s with PASSWORD = '%s' AND LOGIN = true AND SUPERUSER = true",
        roleName, newPassword);
    s.execute(alterStmt);
    Thread.sleep(getSleepInterval());

    // During the first iteration we check tha the changes were applied correctly to the
    // in-memory structures. Then we restart the cluster to verify that the changes were saved
    // to disk and are loaded correctly.
    for (int i = 0; i < 2; i++) {
      verifyRole(roleName, true, true);

      // Verify that we can login.
      checkConnectivity(true, roleName, newPassword, false);

      if (i == 0) {
        miniCluster.restart();
      }
    }
  }
}
