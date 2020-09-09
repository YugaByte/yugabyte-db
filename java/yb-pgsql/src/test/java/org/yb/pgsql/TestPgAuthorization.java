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

import com.google.common.collect.Lists;
import org.hamcrest.CoreMatchers;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.pgsql.cleaners.ClusterCleaner;
import org.yb.pgsql.cleaners.DatabaseCleaner;
import org.yb.pgsql.cleaners.RoleCleaner;
import org.yb.util.MiscUtil.ThrowingRunnable;
import org.yb.util.YBTestRunnerNonTsanOnly;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.sql.Timestamp;
import java.util.Calendar;
import java.util.Date;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.TreeMap;
import java.util.TreeSet;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.Lock;
import java.util.concurrent.locks.ReentrantLock;

import static org.yb.AssertionWrappers.assertEquals;
import static org.yb.AssertionWrappers.assertFalse;
import static org.yb.AssertionWrappers.assertNotEquals;
import static org.yb.AssertionWrappers.assertThat;
import static org.yb.AssertionWrappers.assertTrue;
import static org.yb.AssertionWrappers.fail;

/**
 * Tests for PostgreSQL RBAC.
 */
@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgAuthorization extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgAuthorization.class);

  private static final String PERMISSION_DENIED = "permission denied";

  // Host-based authentication config which requires a password for the pass_role user.
  private static final String CUSTOM_PG_HBA_CONFIG = "" +
      "host    all    pass_role    0.0.0.0/0    password," +
      "host    all    pass_role    ::0/0        password," +
      "host    all    all          0.0.0.0/0    trust," +
      "host    all    all          ::0/0        trust";

  @Override
  protected Map<String, String> getTServerFlags() {
    Map<String, String> flags = super.getTServerFlags();
    flags.put("ysql_hba_conf", CUSTOM_PG_HBA_CONFIG);
    return flags;
  }

  @Override
  protected List<ClusterCleaner> getCleaners() {
    List<ClusterCleaner> cleaners = super.getCleaners();
    cleaners.add(0, new DatabaseCleaner());
    cleaners.add(1, new RoleCleaner());
    return cleaners;
  }

  @Test
  public void testDefaultAuthorization() throws Exception {
    try (Statement statement = connection.createStatement()) {
      // Default users are correctly set.
      assertEquals(TEST_PG_USER, getSessionUser(statement));
      assertEquals(TEST_PG_USER, getCurrentUser(statement));
    }
  }

  @Test
  public void testSessionAuthorization() throws Exception {
    try (Statement statement = connection.createStatement()) {
      // Create some roles.
      statement.execute("CREATE ROLE unprivileged");
      statement.execute("CREATE ROLE su LOGIN SUPERUSER");
      statement.execute("CREATE ROLE some_role LOGIN");
      statement.execute("CREATE ROLE some_group ROLE some_role");
    }

    try (Connection connection = getConnectionBuilder().withUser("su").connect();
         Statement statement = connection.createStatement()) {
      assertEquals("su", getSessionUser(statement));
      assertEquals("su", getCurrentUser(statement));

      // Superuser can set session authorization.
      statement.execute("SET SESSION AUTHORIZATION unprivileged");

      assertEquals("unprivileged", getSessionUser(statement));
      assertEquals("unprivileged", getCurrentUser(statement));

      // Session authorization determines membership.
      runInvalidQuery(statement, "SET ROLE some_role", PERMISSION_DENIED);

      // Original session user is used when determining privileges.
      statement.execute("SET SESSION AUTHORIZATION su");

      assertEquals("su", getSessionUser(statement));
      assertEquals("su", getCurrentUser(statement));

      statement.execute("SET SESSION AUTHORIZATION unprivileged");
      statement.execute("SET SESSION AUTHORIZATION some_role");
      statement.execute("SET ROLE some_group");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_group", getCurrentUser(statement));

      // Session authorization can be reset, and it resets the current role.
      statement.execute("RESET SESSION AUTHORIZATION");

      assertEquals("su", getSessionUser(statement));
      assertEquals("su", getCurrentUser(statement));

      // Using DEFAULT is equivalent to RESET.
      statement.execute("SET SESSION AUTHORIZATION unprivileged");
      statement.execute("SET SESSION AUTHORIZATION DEFAULT");

      assertEquals("su", getSessionUser(statement));
      assertEquals("su", getCurrentUser(statement));

      // SET LOCAL is only valid for current transaction.
      statement.execute("BEGIN");
      statement.execute("SET LOCAL SESSION AUTHORIZATION unprivileged");

      assertEquals("unprivileged", getSessionUser(statement));
      assertEquals("unprivileged", getCurrentUser(statement));

      statement.execute("COMMIT");

      assertEquals("su", getSessionUser(statement));
      assertEquals("su", getCurrentUser(statement));

      // SET SESSION is valid for the entire session.
      statement.execute("BEGIN");
      statement.execute("SET SESSION SESSION AUTHORIZATION unprivileged");
      statement.execute("COMMIT");

      assertEquals("unprivileged", getSessionUser(statement));
      assertEquals("unprivileged", getCurrentUser(statement));
    }

    try (Connection connection = getConnectionBuilder().withUser("su").connect();
         Statement statement = connection.createStatement()) {
      // Users have been reset, since this is a new session.
      assertEquals("su", getSessionUser(statement));
      assertEquals("su", getCurrentUser(statement));
    }

    try (Connection connection = getConnectionBuilder().withUser("some_role").connect();
         Statement statement = connection.createStatement()) {
      statement.execute("SET ROLE some_group");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_group", getCurrentUser(statement));

      // Non-superuser can set session authorization to the role they authenticated as.
      statement.execute("SET SESSION AUTHORIZATION some_role");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      // Non-superuser cannot set session authorization to other roles.
      runInvalidQuery(statement, "SET SESSION AUTHORIZATION some_group", PERMISSION_DENIED);
      runInvalidQuery(statement, "SET SESSION AUTHORIZATION unprivileged", PERMISSION_DENIED);

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      statement.execute("SET ROLE some_group");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_group", getCurrentUser(statement));

      // Non-superuser can reset their session authorization.
      statement.execute("RESET SESSION AUTHORIZATION");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));
    }

    final AtomicInteger state = new AtomicInteger(0);
    final Lock lock = new ReentrantLock();
    final Condition condition = lock.newCondition();

    Thread thread = new Thread(() -> {
      try (Connection connection = getConnectionBuilder().withUser("su").connect();
           Statement statement = connection.createStatement()) {
        // Signal that superuser session has started.
        lock.lock();
        state.incrementAndGet();
        condition.signal();

        // Wait for superuser attribute to be revoked.
        while (state.get() != 2) {
          condition.await();
        }
        lock.unlock();

        // Can still set session authorization, even though "su" is no longer a superuser.
        statement.execute("SET SESSION AUTHORIZATION unprivileged");
        assertEquals("unprivileged", getSessionUser(statement));
        assertEquals("unprivileged", getCurrentUser(statement));
      } catch (Exception e) {
        throw new RuntimeException(e);
      }

      try (Connection connection = getConnectionBuilder().withUser("su").connect();
           Statement statement = connection.createStatement()) {
        // In a new session, "su" can no longer set session authorization.
        runInvalidQuery(statement, "SET SESSION AUTHORIZATION unprivileged", PERMISSION_DENIED);
      } catch (Exception e) {
        throw new RuntimeException(e);
      }
    });
    thread.start();

    // Wait for superuser session to start.
    lock.lock();
    while (state.get() != 1) {
      condition.await();
    }
    lock.unlock();

    try (Statement statement = connection.createStatement()) {
      // Revoke superuser from "su".
      statement.execute("ALTER ROLE su NOSUPERUSER");
    }

    // Signal that superuser attribute has been revoked.
    lock.lock();
    state.incrementAndGet();
    condition.signal();
    lock.unlock();

    thread.join();
  }

  @Test
  public void testRoleChanging() throws Exception {
    try (Statement statement = connection.createStatement()) {
      // Create some roles.
      statement.execute("CREATE ROLE su LOGIN SUPERUSER");
      statement.execute("CREATE ROLE unprivileged");
      statement.execute("CREATE ROLE some_role LOGIN");
      statement.execute("CREATE ROLE some_group LOGIN ROLE some_role");
      statement.execute("CREATE ROLE some_large_group ROLE some_group");
      statement.execute("CREATE ROLE other_group ROLE some_role");
    }

    try (Connection connection = getConnectionBuilder().withUser("some_role").connect();
         Statement statement = connection.createStatement()) {
      // Cannot set roles to those who we aren't a member of.
      runInvalidQuery(statement, "SET ROLE unprivileged", PERMISSION_DENIED);

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      // Can set role to current role.
      statement.execute("SET ROLE some_role");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      // Can set role to parent role.
      statement.execute("SET ROLE some_group");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_group", getCurrentUser(statement));

      // Can reset role.
      statement.execute("RESET ROLE");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      // NONE is equivalent to RESET.
      statement.execute("SET ROLE some_group");
      statement.execute("SET ROLE NONE");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      // Can set role to any role in inheritance tree.
      statement.execute("SET ROLE some_large_group");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_large_group", getCurrentUser(statement));

      // SET privileges depend on session user, not current user.
      statement.execute("SET ROLE other_group");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("other_group", getCurrentUser(statement));

      // SET LOCAL is only valid for current transaction.
      statement.execute("BEGIN");
      statement.execute("SET LOCAL ROLE some_role");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));

      statement.execute("COMMIT");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("other_group", getCurrentUser(statement));

      // SET SESSION is valid for entire session.
      statement.execute("BEGIN");
      statement.execute("SET SESSION ROLE some_group");
      statement.execute("COMMIT");

      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_group", getCurrentUser(statement));
    }

    try (Connection connection = getConnectionBuilder().withUser("some_role").connect();
         Statement statement = connection.createStatement()) {
      // Users have been reset, since this is a new session.
      assertEquals("some_role", getSessionUser(statement));
      assertEquals("some_role", getCurrentUser(statement));
    }

    try (Connection connection = getConnectionBuilder().withUser("some_group").connect();
         Statement statement = connection.createStatement()) {
      // Cannot set roles to those who are a members of the current role.
      runInvalidQuery(statement, "SET ROLE some_role", PERMISSION_DENIED);
    }

    try (Connection connection = getConnectionBuilder().withUser("su").connect();
         Statement statement = connection.createStatement()) {
      // Superuser can set any role.
      statement.execute("SET ROLE unprivileged");
      statement.execute("SET ROLE some_role");
      statement.execute("SET ROLE some_large_group");
    }
  }

  @Test
  public void testAttributes() throws Exception {
    // NOTE: The INHERIT attribute is tested separately in testMembershipInheritance.
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE unprivileged");

      /*
       * SUPERUSER
       */

      // Superuser can create other superusers.
      statement.execute("CREATE ROLE su SUPERUSER");

      // Superuser can alter superuser roles.
      statement.execute("ALTER ROLE su LOGIN");
      statement.execute("ALTER ROLE su NOLOGIN");

      // Superusers can create and drop databases.
      statement.execute("CREATE DATABASE test_su_db");
      statement.execute("DROP DATABASE test_su_db");

      // Superusers can change superuser settings.
      statement.execute("SET deadlock_timeout=2000");
      statement.execute("RESET deadlock_timeout");

      // Superusers cannot login without LOGIN.
      try (Connection ignored = getConnectionBuilder().withUser("su").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("role \"su\" is not permitted to log in")
        );
      }

      statement.execute("CREATE ROLE su_login SUPERUSER LOGIN");
      try (Connection suConnection = getConnectionBuilder().withUser("su_login").connect();
           Statement suStatement = suConnection.createStatement()) {
        suStatement.execute("CREATE TABLE su_login_table(id int)");
      }

      // Superusers bypass ownership and permissions checks.
      statement.execute("DROP TABLE su_login_table");

      // Superusers can read/write/execute server files.
      statement.execute("CREATE TABLE files(filename text)");
      statement.execute("SELECT pg_read_file('/non/existent/file', 0, 0, true)");
      statement.execute("COPY (SELECT * FROM pg_roles) TO '/dev/null'");
      statement.execute("COPY files FROM PROGRAM 'ls /usr/bin'");

      /*
       * CREATEDB
       */

      statement.execute("CREATE ROLE cdb_role CREATEDB");

      withRole(statement, "cdb_role", () -> {
        // User with CREATEDB can create a database.
        statement.execute("CREATE DATABASE cdb_role_db");
        statement.execute("DROP DATABASE cdb_role_db");
      });

      withRole(statement, "unprivileged", () -> {
        // Unprivileged users cannot create databases.
        runInvalidQuery(statement, "CREATE DATABASE cdb_role_db", PERMISSION_DENIED);
      });

      /*
       * CREATEROLE
       */

      statement.execute("CREATE ROLE cr_role CREATEROLE");

      withRole(statement, "cr_role", () -> {
        // User with CREATEROLE can create a role.
        statement.execute("CREATE ROLE cr_created_role");
        statement.execute("DROP ROLE cr_created_role");

        // Non-superusers cannot create superuser roles.
        runInvalidQuery(
            statement,
            "CREATE ROLE cr_created_role SUPERUSER",
            "must be superuser to create superusers"
        );

        // Non-superusers cannot alter superuser roles.
        runInvalidQuery(
            statement,
            "ALTER ROLE su LOGIN",
            "must be superuser to alter superusers"
        );
      });

      withRole(statement, "unprivileged", () -> {
        // Unprivileged users cannot create roles.
        runInvalidQuery(statement, "CREATE ROLE cr_created_role", PERMISSION_DENIED);
      });

      /*
       * LOGIN
       */

      statement.execute("CREATE ROLE login_user LOGIN");

      // Users with LOGIN can connect directly.
      try (Connection ignored = getConnectionBuilder().withUser("login_user").connect()) {
        // No-op.
      }

      // Users without LOGIN cannot connect directly.
      try (Connection ignored = getConnectionBuilder().withUser("unprivileged").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("role \"unprivileged\" is not permitted to log in")
        );
      }

      /*
       * REPLICATION
       */

      // Replication is not currently supported, but we should still be able to add the attribute.
      statement.execute("CREATE ROLE replication_user REPLICATION");

      /*
       * BYPASSRLS
       */

      // RLS is not currently supported, but we should still be able to add the attribute.
      statement.execute("CREATE ROLE bypassrls_user BYPASSRLS");

      /*
       * CONNECTION LIMIT
       */

      statement.execute("CREATE ROLE limit_role LOGIN CONNECTION LIMIT 2");

      ConnectionBuilder limitRoleUserConnBldr = getConnectionBuilder().withUser("limit_role");
      try (Connection ignored1 = limitRoleUserConnBldr.connect()) {
        try (Connection connection2 = limitRoleUserConnBldr.connect()) {
          // Third concurrent connection causes error.
          try (Connection ignored3 = limitRoleUserConnBldr.connect()) {
            fail("Expected third login attempt to fail");
          } catch (SQLException sqle) {
            assertThat(
                sqle.getMessage(),
                CoreMatchers.containsString("too many connections for role \"limit_role\"")
            );
          }

          // Close second connection.
          connection2.close();

          // New connection now succeeds.
          try (Connection ignored2 = limitRoleUserConnBldr.connect()) {
            // No-op.
          }
        }
      }

      /*
       * PASSWORD
       */

      // Create role with password.
      statement.execute("DROP ROLE IF EXISTS pass_role");
      statement.execute("CREATE ROLE pass_role LOGIN PASSWORD 'pass1'");

      // Password is encrypted, despite not being specified.
      ResultSet password_result = statement.executeQuery(
          "SELECT rolpassword FROM pg_authid WHERE rolname='pass_role'");
      password_result.next();
      String password_hash = password_result.getString(1);
      assertNotEquals(password_hash, "");
      assertNotEquals(password_hash, "pass1");

      ConnectionBuilder passRoleUserConnBldr = getConnectionBuilder().withUser("pass_role");

      // Can login with password.
      try (Connection ignored = passRoleUserConnBldr.withPassword("pass1").connect()) {
        // No-op.
      }

      // Cannot login without password.
      try (Connection ignored = passRoleUserConnBldr.connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("no password was provided")
        );
      }

      // Cannot login with incorrect password.
      try (Connection ignored = passRoleUserConnBldr.withPassword("wrong").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("password authentication failed for user \"pass_role\"")
        );
      }

      // Password does not imply login.
      statement.execute("DROP ROLE IF EXISTS pass_role");
      statement.execute("CREATE ROLE pass_role PASSWORD 'pass1'");
      try (Connection ignored = passRoleUserConnBldr.withPassword("pass1").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("role \"pass_role\" is not permitted to log in")
        );
      }

      /*
       * ENCRYPTED PASSWORD
       */

      // Create role with encrypted password.
      statement.execute("DROP ROLE IF EXISTS pass_role");
      statement.execute("CREATE ROLE pass_role LOGIN ENCRYPTED PASSWORD 'pass2'");

      // Password is encrypted.
      password_result = statement.executeQuery(
          "SELECT rolpassword FROM pg_authid WHERE rolname='pass_role'");
      password_result.next();
      password_hash = password_result.getString(1);
      assertNotEquals(password_hash, "");
      assertNotEquals(password_hash, "pass2");

      // Can login with password.
      try (Connection ignored = passRoleUserConnBldr.withPassword("pass2").connect()) {
        // No-op.
      }

      // Cannot login without password.
      try (Connection ignored = passRoleUserConnBldr.connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("no password was provided")
        );
      }

      // Cannot login with incorrect password.
      try (Connection ignored = passRoleUserConnBldr.withPassword("wrong").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("password authentication failed for user \"pass_role\"")
        );
      }

      /*
       * UNENCRYPTED PASSWORD
       */

      // Cannot create users with unencrypted passwords.
      statement.execute("DROP ROLE IF EXISTS pass_role");
      runInvalidQuery(
          statement,
          "CREATE ROLE pass_role LOGIN UNENCRYPTED PASSWORD 'pass'",
          "UNENCRYPTED PASSWORD is no longer supported"
      );

      /*
       * VALID UNTIL
       */

      Calendar cal = Calendar.getInstance();
      cal.setTime(new Date());
      cal.add(Calendar.SECOND, 10);
      Timestamp expiriationTime = new Timestamp(cal.getTime().getTime());

      statement.execute("DROP ROLE IF EXISTS pass_role");
      statement.execute("CREATE ROLE pass_role LOGIN PASSWORD 'password' " +
          "VALID UNTIL '" + expiriationTime.toString() + "'");

      // Can connect now.
      try (Connection ignored = passRoleUserConnBldr.withPassword("password").connect()) {
        // No-op.
      }

      // Wait until after the expiration time.
      Thread.sleep(11000);

      // Password is no longer valid.
      try (Connection ignored = passRoleUserConnBldr.withPassword("password").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("password authentication failed for user \"pass_role\"")
        );
      }

      /*
       * IN ROLE
       */

      // Create group.
      statement.execute("CREATE ROLE group_role NOLOGIN");

      statement.execute("CREATE ROLE in_role_user IN ROLE group_role");

      withRole(statement, "in_role_user", () -> {
        // in_role_user role is immediately a member of group_role.
        statement.execute("SET ROLE group_role");
      });

      /*
       * IN GROUP
       */

      statement.execute("CREATE ROLE in_group_user IN GROUP group_role");

      withRole(statement, "in_group_user", () -> {
        // in_group_user role is immediately a member of group_role.
        statement.execute("SET ROLE group_role");
      });

      /*
       * ROLE
       */

      // Create a roles to add to other roles.
      statement.execute("CREATE ROLE role_to_add");
      statement.execute("CREATE ROLE other_role_to_add");

      statement.execute("CREATE ROLE add_role_role ROLE role_to_add");

      withRole(statement, "role_to_add", () -> {
        // role_to_add has been automatically added to the group.
        statement.execute("SET ROLE add_role_role");
        statement.execute("RESET ROLE");

        // role_to_add cannot add new members to the group.
        runInvalidQuery(
            statement,
            "GRANT add_role_role TO other_role_to_add",
            "must have admin option on role \"add_role_role\""
        );
      });

      /*
       * USER
       */

      statement.execute("CREATE ROLE add_user_role USER role_to_add");

      withRole(statement, "role_to_add", () -> {
        // role_to_add has been automatically added to the group.
        statement.execute("SET ROLE add_user_role");
        statement.execute("RESET ROLE");

        // role_to_add cannot add new members to the group.
        runInvalidQuery(
            statement,
            "GRANT add_user_role TO other_role_to_add",
            "must have admin option on role \"add_user_role\""
        );
      });

      /*
       * ADMIN
       */

      statement.execute("CREATE ROLE add_admin_role ADMIN role_to_add");

      withRole(statement, "role_to_add", () -> {
        // role_to_add has been automatically added to the group.
        statement.execute("SET ROLE add_admin_role");
        statement.execute("RESET ROLE");

        // role_to_add can add new members to the group.
        statement.execute("GRANT add_admin_role to other_role_to_add");
      });

      /*
       * SYSID
       */

      // This attribute is ignored, but we should still be able to add it.
      statement.execute("CREATE ROLE sysid_role SYSID 12345");
    }
  }

  @Test
  public void testAlterAttributes() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE can_login LOGIN");
      statement.execute("CREATE ROLE cannot_login");

      // Alter roles to switch attributes.
      statement.execute("ALTER ROLE can_login NOLOGIN");
      statement.execute("ALTER ROLE cannot_login LOGIN");

      // Role "can_login" can no longer login.
      try (Connection ignored = getConnectionBuilder().withUser("can_login").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("role \"can_login\" is not permitted to log in")
        );
      }

      // Role "cannot_login" can now login.
      try (Connection ignored = getConnectionBuilder().withUser("cannot_login").connect()) {
        // No-op.
      }
    }
  }

  @Test
  public void testSingleLevelMembershipInheritance() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE inh_role LOGIN INHERIT");
      statement.execute("CREATE ROLE no_inh_role LOGIN NOINHERIT");

      statement.execute("CREATE ROLE create_role_group CREATEROLE ROLE inh_role, no_inh_role");

      statement.execute("CREATE ROLE role_with_config ROLE inh_role, no_inh_role");
      statement.execute("ALTER ROLE role_with_config SET search_path='some path'");

      statement.execute("CREATE ROLE role_with_privileges ROLE inh_role, no_inh_role");
      statement.execute("CREATE TABLE test_table(id int)");
      statement.execute("GRANT SELECT ON TABLE test_table TO role_with_privileges");
    }

    try (Connection connection = getConnectionBuilder().withUser("no_inh_role").connect();
         Statement statement = connection.createStatement()) {
      // NOINHERIT user does not inherit attributes.
      runInvalidQuery(statement, "CREATE ROLE test", PERMISSION_DENIED);

      // NOINHERIT user does not inherit config variables.
      assertQuery(statement, "SHOW search_path", new Row("\"$user\", public"));

      // NOINHERIT user does not inherit privileges.
      runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
    }

    try (Connection connection = getConnectionBuilder().withUser("inh_role").connect();
         Statement statement = connection.createStatement()) {
      // INHERIT user does not inherit attributes.
      runInvalidQuery(statement, "CREATE ROLE test", PERMISSION_DENIED);

      // INHERIT user does not inherit config variables.
      assertQuery(statement, "SHOW search_path", new Row("\"$user\", public"));

      // INHERIT user does inherit privileges.
      assertQuery(statement, "SELECT * FROM test_table");
    }
  }

  @Test
  public void testMultiLevelMembershipInheritance() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE inh_role LOGIN INHERIT");

      statement.execute("CREATE ROLE inh_group INHERIT ROLE inh_role");
      statement.execute("CREATE ROLE no_inh_group NOINHERIT ROLE inh_role");

      statement.execute("CREATE ROLE role_with_privileges1 ROLE inh_group");
      statement.execute("CREATE TABLE test_table1(id int)");
      statement.execute("GRANT SELECT ON TABLE test_table1 TO role_with_privileges1");

      statement.execute("CREATE ROLE role_with_privileges2 ROLE no_inh_group");
      statement.execute("CREATE TABLE test_table2(id int)");
      statement.execute("GRANT SELECT ON TABLE test_table2 TO role_with_privileges2");

      withRole(statement, "inh_role", () -> {
        // Base role inherits privileges from parent of INHERIT group.
        assertQuery(statement, "SELECT * FROM test_table1");

        // Base role does not inherit privileges from parent of NOINHERIT group.
        runInvalidQuery(statement, "SELECT * FROM test_table2", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testDefaultRoles() throws Exception {
    RoleSet roles = new RoleSet("unprivileged", "pg_read_all_settings", "pg_read_all_stats",
        "pg_stat_scan_tables", "pg_signal_backend", "pg_read_server_files", "pg_write_server_files",
        "pg_execute_server_program", "pg_monitor");

    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE unprivileged");

      /*
       * pg_read_all_settings
       */

      withRole(statement, "pg_read_all_settings", () -> {
        // Can read superuser-only settings.
        assertTrue(statement.executeQuery(
            "SELECT * FROM pg_settings WHERE name='dynamic_library_path'").next());

        // Cannot change superuser settings.
        runInvalidQuery(statement, "SET dynamic_library_path='/somedir'", PERMISSION_DENIED);
        runInvalidQuery(statement, "RESET dynamic_library_path", PERMISSION_DENIED);
      });

      withRoles(statement, roles.excluding("pg_read_all_settings", "pg_monitor"), () -> {
        // Other roles cannot read superuser-only settings.
        assertFalse(statement.executeQuery(
            "SELECT * FROM pg_settings WHERE name='dynamic_library_path'").next());

        // Other roles cannot change superuser settings.
        runInvalidQuery(statement, "SET dynamic_library_path='/somedir'", PERMISSION_DENIED);
        runInvalidQuery(statement, "RESET dynamic_library_path", PERMISSION_DENIED);
      });

      /*
       * pg_read_all_stats
       */

      withRole(statement, "pg_read_all_stats", () -> {
        // Can read all statistics views without censorship.
        statement.execute("SELECT query FROM pg_stat_activity");
        ResultSet resultSet = statement.getResultSet();
        while (resultSet.next()) {
          assertNotEquals(resultSet.getString(1), "<insufficient privilege>");
        }
      });

      // Other roles (except pg_monitor) have some statistics censored.
      withRoles(statement, roles.excluding("pg_read_all_stats", "pg_monitor"), () -> {
        statement.execute("SELECT query FROM pg_stat_activity");
        ResultSet results = statement.getResultSet();
        while (results.next()) {
          assertEquals(results.getString(1), "<insufficient privilege>");
        }
      });

      /*
       * pg_stat_scan_tables
       */

      // I am unaware of any monitoring functions in vanilla postgres which require the
      // pg_stat_scan_tables role, so just test that it exists.
      statement.execute("SET SESSION AUTHORIZATION pg_stat_scan_tables");
      statement.execute("RESET SESSION AUTHORIZATION");

      /*
       * pg_signal_backend
       */

      // Create a new non-superuser user, whose queries we can kill.
      statement.execute("CREATE ROLE test_user LOGIN;");

      final AtomicInteger testBackendPid = new AtomicInteger(-1);
      final Lock lock = new ReentrantLock();
      final Condition condition = lock.newCondition();

      new Thread(() -> {
        try (Connection testConnection = getConnectionBuilder().withUser("test_user").connect();
             Statement testStatement = testConnection.createStatement()) {
          // Get pid of backend process for this connection.
          ResultSet resultSet = testStatement.executeQuery("SELECT pg_backend_pid()");
          resultSet.next();

          lock.lock();
          testBackendPid.set(resultSet.getInt(1));
          condition.signal();
          lock.unlock();

          // Execute a long-running query.
          runInvalidQuery(
              testStatement,
              "SELECT pg_sleep(10)",
              "canceling statement due to user request"
          );

        } catch (Exception e) {
          throw new RuntimeException(e);
        }
      }).start();

      // Wait for backend process id of the test connection.
      lock.lock();
      while (testBackendPid.get() < 0) {
        condition.await();
      }
      lock.unlock();

      // Allow enough time for the long-running query to start, but not finish.
      Thread.sleep(500);

      // Other roles cannot cancel the test user's query.
      withRoles(statement, roles.excluding("pg_signal_backend"), () -> runInvalidQuery(
          statement,
          "SELECT pg_cancel_backend(" + testBackendPid + ")",
          "must be a member of the role whose query is being canceled or member" +
              " of pg_signal_backend"
      ));

      // pg_signal_backend can cancel the query.
      withRole(statement, "pg_signal_backend",
          () -> statement.execute("SELECT pg_cancel_backend(" + testBackendPid + ")"));

      // No default roles can use these signals.
      withRoles(statement, roles, () -> {
        runInvalidQuery(statement, "SELECT pg_reload_conf()", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT pg_rotate_logfile()", PERMISSION_DENIED);
      });

      /*
       * pg_read_server_files
       */

      // Grant execute on pg_read_file to all.
      statement.execute(
          "GRANT EXECUTE ON FUNCTION pg_read_file(text, bigint, bigint, boolean) TO PUBLIC");

      // Create table to copy into.
      statement.execute("CREATE TABLE copy_test(id int)");
      statement.execute("GRANT ALL ON TABLE copy_test TO PUBLIC");

      withRole(statement, "pg_read_server_files", () -> {
        // Can read files by absolute path.
        statement.execute("SELECT pg_read_file('/non/existent/file', 0, 0, true)");

        // Can copy from files.
        statement.execute("COPY copy_test FROM '/dev/null'");
      });

      withRoles(statement, roles.excluding("pg_read_server_files"), () -> {
        // Other roles cannot read files by absolute path.
        runInvalidQuery(
            statement,
            "SELECT pg_read_file('/non/existent/file', 0, 0, true)",
            "absolute path not allowed"
        );

        // Other roles cannot copy from files.
        runInvalidQuery(
            statement,
            "COPY copy_test FROM '/dev/null'",
            "must be superuser or a member of the pg_read_server_files role to COPY from a file"
        );
      });

      /*
       * pg_write_server_files
       */

      withRole(statement, "pg_write_server_files", () -> {
        // Can copy to files.
        statement.execute("COPY (SELECT * FROM pg_roles) TO '/dev/null'");
      });

      withRoles(statement, roles.excluding("pg_write_server_files"), () -> {
        // Other roles cannot copy to files.
        runInvalidQuery(
            statement,
            "COPY (SELECT * FROM pg_roles) TO '/dev/null'",
            "must be superuser or a member of the pg_write_server_files role to COPY to a file"
        );
      });

      /*
       * pg_execute_server_program
       */

      // Create table to copy into.
      statement.execute("CREATE TABLE files(filename text)");
      statement.execute("GRANT ALL ON TABLE files TO PUBLIC");

      withRole(statement, "pg_execute_server_program", () -> {
        // Can execute a command on the server.
        statement.execute("COPY files FROM PROGRAM 'ls /usr/bin'");
      });

      withRoles(statement, roles.excluding("pg_execute_server_program"), () -> {
        // Other users cannot execute commands.
        runInvalidQuery(
            statement,
            "COPY files FROM PROGRAM 'ls /usr/bin'",
            "must be superuser or a member of the pg_execute_server_program role to COPY to " +
                "or from an external program"
        );
      });

      /*
       * pg_monitor
       */

      withRole(statement, "pg_monitor", () -> {
        // Can execute monitoring functions.
        statement.execute("SELECT pg_ls_waldir()");

        // Member of pg_read_all_settings, pg_read_all_stats, and pg_stat_scan_tables.
        statement.execute("SET ROLE pg_read_all_settings");
        statement.execute("SET ROLE pg_read_all_stats");
        statement.execute("SET ROLE pg_stat_scan_tables");

        // Not a member of any other roles.
        runInvalidQuery(statement, "SET ROLE pg_signal_backend", PERMISSION_DENIED);
        runInvalidQuery(statement, "SET ROLE pg_read_server_files", PERMISSION_DENIED);
        runInvalidQuery(statement, "SET ROLE pg_write_server_files", PERMISSION_DENIED);
        runInvalidQuery(statement, "SET ROLE pg_execute_server_program", PERMISSION_DENIED);
      });

      // Other roles cannot use monitoring functions.
      withRoles(statement, roles.excluding("pg_monitor"),
          () -> runInvalidQuery(statement, "SELECT pg_ls_waldir()", PERMISSION_DENIED));
    }
  }

  @Test
  public void testAlterRoleConfiguration() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role LOGIN");

      try (Connection connection1 = getConnectionBuilder().withUser("test_role").connect();
           Statement statement1 = connection1.createStatement()) {
        assertQuery(statement1, "SHOW search_path", new Row("\"$user\", public"));

        // Set configuration variable for "test_role".
        statement.execute("ALTER ROLE test_role SET search_path TO 'some path'");

        waitForTServerHeartbeat();

        // Change is not visible for currently open sessions.
        assertQuery(statement1, "SHOW search_path", new Row("\"$user\", public"));
      }

      try (Connection connection1 = getConnectionBuilder().withUser("test_role").connect();
           Statement statement1 = connection1.createStatement()) {
        // Change is visible in new sessions.
        assertQuery(statement1, "SHOW search_path", new Row("\"some path\""));
      }

      try (Connection connection1 = getConnectionBuilder().connect();
           Statement statement1 = connection1.createStatement()) {
        withRole(statement1, "test_role", () -> {
          // Change is not visible if we didn't login as "test_role".
          assertQuery(statement1, "SHOW search_path", new Row("\"$user\", public"));
        });
      }

      statement.execute("ALTER ROLE test_role RESET search_path");

      try (Connection connection1 = getConnectionBuilder().withUser("test_role").connect();
           Statement statement1 = connection1.createStatement()) {
        // Search path is reset.
        assertQuery(statement1, "SHOW search_path", new Row("\"$user\", public"));
      }
    }
  }

  @Test
  public void testAlterRoleConfigurationInDatabase() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role LOGIN");

      // Set configuration variable for "test_role" in "yugabyte".
      statement.execute("ALTER ROLE test_role IN DATABASE yugabyte SET search_path TO 'some path'");

      try (Connection connection1 = getConnectionBuilder().withUser("test_role").connect();
           Statement statement1 = connection1.createStatement()) {
        // Change is visible in postgres database.
        assertQuery(statement1, "SHOW search_path", new Row("\"some path\""));
      }

      statement.execute("CREATE DATABASE tdb");
      statement.execute("ALTER DATABASE tdb OWNER TO test_role");

      try (Connection connection1 = getConnectionBuilder().withDatabase("tdb")
          .withUser("test_role").connect();
           Statement statement1 = connection1.createStatement()) {
        // Change is not visible in other databases.
        assertQuery(statement1, "SHOW search_path", new Row("\"$user\", public"));
      }
    }
  }

  @Test
  public void testAlterRoleConfigurationForAll() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role LOGIN");
      statement.execute("CREATE ROLE other_role LOGIN");

      // Set configuration variable for all roles.
      statement.execute("ALTER ROLE ALL SET search_path TO 'some path'");

      // Change is visible for both users.
      try (Connection connection1 = getConnectionBuilder().withUser("test_role").connect();
           Statement statement1 = connection1.createStatement()) {
        assertQuery(statement1, "SHOW search_path", new Row("\"some path\""));
      }
      try (Connection connection1 = getConnectionBuilder().withUser("other_role").connect();
           Statement statement1 = connection1.createStatement()) {
        assertQuery(statement1, "SHOW search_path", new Row("\"some path\""));
      }
    }
  }

  @Test
  public void testGroupMembershipCycles() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE r1");
      statement.execute("CREATE ROLE r2 ROLE r1");
      statement.execute("CREATE ROLE r3 ROLE r2");
      statement.execute("CREATE ROLE r4 ROLE r3");

      runInvalidQuery(statement, "GRANT r1 TO r4", "role \"r1\" is a member of role \"r4\"");
    }
  }

  @Test
  public void testDatabasePrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE DATABASE test_db");
      ConnectionBuilder tdbConnBldr = getConnectionBuilder().withDatabase("test_db");

      // Remove any default privileges.
      statement.execute("REVOKE ALL ON DATABASE test_db FROM PUBLIC");

      /*
       * CREATE
       */

      statement.execute("CREATE ROLE create_role LOGIN");
      statement.execute("GRANT CREATE ON DATABASE test_db TO create_role");

      try (Connection connection1 = tdbConnBldr.connect();
           Statement statement1 = connection1.createStatement()) {
        withRole(statement1, "create_role", () -> {
          // Can create schemas.
          statement1.execute("CREATE SCHEMA test_schema");

          // Cannot create temporary tables.
          runInvalidQuery(statement1, "CREATE TEMP TABLE test_table(id int)", PERMISSION_DENIED);
        });
      }

      // Cannot connect directly to the database.
      try (Connection ignored = tdbConnBldr.withUser("create_role").connect()) {
        fail("Expected connection attempt to fail");
      } catch (SQLException sqle) {
        assertThat(sqle.getMessage(), CoreMatchers.containsString(PERMISSION_DENIED));
      }

      /*
       * CONNECT
       */

      statement.execute("CREATE ROLE connect_role LOGIN");
      statement.execute("GRANT CONNECT ON DATABASE test_db TO connect_role");

      // Can connect directly to the database.
      try (Connection connection1 = tdbConnBldr.withUser("connect_role").connect();
           Statement statement1 = connection1.createStatement()) {
        // Cannot create schemas.
        runInvalidQuery(statement1, "CREATE SCHEMA other_schema", PERMISSION_DENIED);

        // Cannot create temporary tables.
        runInvalidQuery(statement1, "CREATE TEMP TABLE test_table(id int)", PERMISSION_DENIED);
      }

      /*
       * TEMP
       */

      statement.execute("CREATE ROLE temp_role LOGIN");
      statement.execute("GRANT TEMP ON DATABASE test_db TO temp_role");

      try (Connection connection1 = tdbConnBldr.connect();
           Statement statement1 = connection1.createStatement()) {
        withRole(statement1, "temp_role", () -> {
          // Can create temporary tables.
          statement.execute("CREATE TEMP TABLE test_table(id int)");

          // Cannot create schemas.
          runInvalidQuery(statement1, "CREATE SCHEMA other_schema", PERMISSION_DENIED);
        });
      }

      // Cannot connect directly to the database.
      try (Connection ignored = tdbConnBldr.withUser("temp_role").connect()) {
        fail("Expected connection attempt to fail");
      } catch (SQLException sqle) {
        assertThat(sqle.getMessage(), CoreMatchers.containsString(PERMISSION_DENIED));
      }
    }
  }

  @Test
  public void testSchemaPrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SCHEMA test_schema");

      // Remove any default privileges.
      statement.execute("REVOKE ALL ON SCHEMA test_schema FROM PUBLIC");

      /*
       * CREATE
       */

      statement.execute("CREATE ROLE create_role");
      statement.execute("GRANT CREATE ON SCHEMA test_schema TO create_role");

      withRole(statement, "create_role", () -> {
        // Can create objects in schema.
        statement.execute("CREATE TABLE test_schema.test_table(id int)");

        // Cannot rename objects without USAGE.
        runInvalidQuery(
            statement,
            "ALTER TABLE test_schema.test_table RENAME TO new_table",
            PERMISSION_DENIED
        );

        withRole(statement, TEST_PG_USER, () ->
            statement.execute("GRANT USAGE ON SCHEMA test_schema TO create_role"));

        // Can rename owned objects.
        statement.execute("ALTER TABLE test_schema.test_table RENAME TO new_table");

        withRole(statement, TEST_PG_USER, () ->
            statement.execute("REVOKE CREATE ON SCHEMA test_schema FROM create_role"));

        // Revoking CREATE removes rename privileges, even if table is owned.
        runInvalidQuery(
            statement,
            "ALTER TABLE test_schema.new_table RENAME TO test_table",
            PERMISSION_DENIED
        );
      });

      /*
       * USAGE
       */

      statement.execute("CREATE ROLE usage_role");
      statement.execute("GRANT USAGE ON SCHEMA test_schema TO usage_role");

      withRole(statement, "usage_role", () -> {
        // Cannot create objects in schema.
        runInvalidQuery(
            statement,
            "CREATE TABLE test_schema.other_table(id int)",
            PERMISSION_DENIED
        );

        withRole(statement, "create_role", () ->
            statement.execute("GRANT SELECT ON TABLE test_schema.new_table TO usage_role"));

        // Can use existing objects (with permission).
        statement.execute("SELECT * FROM test_schema.new_table");
      });
    }
  }

  @Test
  public void testDomainPrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE DOMAIN nn_int AS int NOT NULL");

      // Revoke any default privileges.
      statement.execute("REVOKE ALL ON DOMAIN nn_int FROM PUBLIC");

      statement.execute("CREATE ROLE test_role");

      withRole(statement, "test_role", () -> {
        // Cannot create objects using domain without USAGE.
        runInvalidQuery(statement, "CREATE TABLE test_table(id nn_int)", PERMISSION_DENIED);
      });

      statement.execute("GRANT USAGE ON DOMAIN nn_int TO test_role");

      withRole(statement, "test_role", () -> {
        // Can create objects using domain after grant.
        statement.execute("CREATE TABLE test_table(id nn_int)");
      });

      statement.execute("REVOKE USAGE ON DOMAIN nn_int FROM test_role");

      withRole(statement, "test_role", () -> {
        // Can still select type without USAGE.
        statement.execute("SELECT * FROM test_table");
      });
    }
  }

  @Test
  public void testSequencePrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE SEQUENCE test_seq");

      // Revoke any default privileges.
      statement.execute("REVOKE ALL ON SEQUENCE test_seq FROM PUBLIC");

      /*
       * USAGE
       */

      statement.execute("CREATE ROLE usage_role");
      statement.execute("GRANT USAGE ON SEQUENCE test_seq TO usage_role");

      withRole(statement, "usage_role", () -> {
        // Can use "currval" and "nextval".
        statement.execute("SELECT nextval('test_seq')");
        statement.execute("SELECT currval('test_seq')");

        // Cannot use "setval".
        runInvalidQuery(statement, "SELECT setval('test_seq', 3)", PERMISSION_DENIED);

        // Cannot select.
        runInvalidQuery(statement, "SELECT * FROM test_seq", PERMISSION_DENIED);
      });

      /*
       * SELECT
       */

      statement.execute("CREATE ROLE select_role");
      statement.execute("GRANT SELECT ON SEQUENCE test_seq TO select_role");

      withRole(statement, "select_role", () -> {
        // Can use "currval".
        statement.execute("SELECT currval('test_seq')");

        // Cannot use "nextval" or "setval".
        runInvalidQuery(statement, "SELECT nextval('test_seq')", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT setval('test_seq', 3)", PERMISSION_DENIED);

        // Can select sequence.
        statement.execute("SELECT * FROM test_seq");
      });

      /*
       * UPDATE
       */

      statement.execute("CREATE ROLE update_role");
      statement.execute("GRANT UPDATE ON SEQUENCE test_seq TO update_role");

      withRole(statement, "update_role", () -> {
        // Can use "nextval" and "setval".
        statement.execute("SELECT nextval('test_seq')");
        statement.execute("SELECT setval('test_seq', 3)");

        // Cannot use "curval".
        runInvalidQuery(statement, "SELECT currval('test_seq')", PERMISSION_DENIED);

        // Cannot select.
        runInvalidQuery(statement, "SELECT * FROM test_seq", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testViewPrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      statement.execute("CREATE TABLE test_view_back(a int, b int, c int, PRIMARY KEY (a))");
      statement.execute("CREATE VIEW test_view AS SELECT a, c FROM test_view_back");

      /*
       * SELECT
       */

      statement.execute("GRANT SELECT ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Select is allowed.
        statement.execute("SELECT * FROM test_view");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, c) VALUES (1, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * INSERT
       */

      statement.execute("GRANT INSERT ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Insert is allowed.
        statement.execute("INSERT INTO test_view(a, c) VALUES (1, 3)");

        // Others are not.
        runInvalidQuery(statement, "SELECT * FROM test_view", PERMISSION_DENIED);
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * UPDATE
       */

      statement.execute("GRANT UPDATE ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Update is allowed.
        statement.execute("UPDATE test_view SET c = 2");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, c) VALUES (1, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * DELETE, SELECT
       */

      statement.execute("GRANT DELETE, SELECT ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Delete with selection is allowed.
        statement.execute("DELETE FROM test_view WHERE a = 2");

        // Select is allowed.
        statement.execute("SELECT * FROM test_view");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, c) VALUES (1, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * UPDATE, SELECT
       */

      statement.execute("GRANT UPDATE ON TABLE test_view TO test_role");
      statement.execute("GRANT SELECT ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Update with selection is allowed.
        statement.execute("UPDATE test_view SET c = 2 WHERE a = 1");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, c) VALUES (1, 3)",
            PERMISSION_DENIED
        );
      });

      statement.execute("REVOKE SELECT ON TABLE test_view FROM test_role");
      withRole(statement, "test_role", () -> {
        // Can no longer perform updates with selection.
        runInvalidQuery(
            statement,
            "UPDATE test_view SET c = 2 WHERE a = 1",
            PERMISSION_DENIED
        );

        // Can still perform simple updates.
        statement.execute("UPDATE test_view SET c = 2");
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * ALL
       */

      statement.execute("GRANT ALL ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // All are allowed.
        statement.execute("SELECT * FROM test_view");
        statement.execute("INSERT INTO test_view(a, c) VALUES (2, 3)");
        statement.execute("UPDATE test_view SET c = 2");
        statement.execute("DELETE FROM test_view WHERE a = 2");
      });
    }
  }

  @Test
  public void testViewColumnPrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      statement.execute("CREATE TABLE test_view_back(a int, b int, c int, PRIMARY KEY (a))");
      statement.execute("CREATE VIEW test_view AS SELECT a, b, c FROM test_view_back");

      /*
       * SELECT
       */

      statement.execute("GRANT SELECT(a) ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Select from "a" is allowed.
        statement.execute("SELECT a FROM test_view");

        // Others are not.
        runInvalidQuery(statement, "SELECT b FROM test_view", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT c FROM test_view", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM test_view", PERMISSION_DENIED);
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      statement.execute("GRANT SELECT(a, b, c) ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Select is allowed.
        statement.execute("SELECT * FROM test_view");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });

      // Revoke a single column.
      statement.execute("REVOKE SELECT(b) ON TABLE test_view FROM test_role");
      withRole(statement, "test_role", () -> {
        // Select from "a" and "c" are allowed.
        statement.execute("SELECT a FROM test_view");
        statement.execute("SELECT c FROM test_view");
        statement.execute("SELECT a, c FROM test_view");

        // Others are not.
        runInvalidQuery(statement, "SELECT b FROM test_view", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM test_view", PERMISSION_DENIED);
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * INSERT
       */

      statement.execute("GRANT INSERT(a) ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Insert "a" is allowed.
        statement.execute("INSERT INTO test_view(a) VALUES (2)");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(b) VALUES (4)",
            PERMISSION_DENIED
        );
        runInvalidQuery(statement, "SELECT * FROM test_view", PERMISSION_DENIED);
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * UPDATE
       */

      statement.execute("GRANT UPDATE(c) ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Update is allowed.
        statement.execute("UPDATE test_view SET c = 2");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "UPDATE test_view SET c = 2 WHERE a = 1",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "UPDATE test_view SET a = 2, b = 2, c = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "UPDATE test_view SET b = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * UPDATE, SELECT
       */

      statement.execute("GRANT UPDATE(c), SELECT(a) ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // Update with selection is allowed.
        statement.execute("UPDATE test_view SET c = 2 WHERE a = 1");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "UPDATE test_view SET a = 2, b = 2, c = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "UPDATE test_view SET b = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(c) VALUES (2)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_view FROM test_role");

      /*
       * ALL
       */

      statement.execute("GRANT ALL(a, b) ON TABLE test_view TO test_role");
      withRole(statement, "test_role", () -> {
        // All on "a" and "b" are allowed.
        statement.execute("SELECT a FROM test_view");
        statement.execute("INSERT INTO test_view(a, b) VALUES (3, 4)");
        statement.execute("UPDATE test_view SET a = 2 WHERE b = 3");

        // Others are not.
        runInvalidQuery(statement, "SELECT b, c FROM test_view", PERMISSION_DENIED);
        runInvalidQuery(
            statement,
            "INSERT INTO test_view(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
        runInvalidQuery(statement, "UPDATE test_view SET c = 3", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testTemporaryTablePrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      statement.execute("CREATE TEMP TABLE test_table(a int, b int, c int, PRIMARY KEY (a))");

      /*
       * SELECT
       */

      statement.execute("GRANT SELECT ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Select is allowed.
        statement.execute("SELECT * FROM test_table");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * INSERT
       */

      statement.execute("GRANT INSERT ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Insert is allowed.
        statement.execute("INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)");

        // Others are not.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * UPDATE
       */

      statement.execute("GRANT UPDATE ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Update is allowed.
        statement.execute("UPDATE test_table SET c = 2");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * DELETE, SELECT
       */

      statement.execute("GRANT DELETE, SELECT ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Delete with selection is allowed.
        statement.execute("DELETE FROM test_table WHERE a = 2");

        // Select is allowed.
        statement.execute("SELECT * FROM test_table");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * TRUNCATE
       */

      statement.execute("GRANT TRUNCATE ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Truncate is allowed.
        statement.execute("TRUNCATE TABLE test_table");

        // Others are not.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * UPDATE, SELECT
       */

      statement.execute("GRANT UPDATE ON TABLE test_table TO test_role");
      statement.execute("GRANT SELECT ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Update with selection is allowed.
        statement.execute("UPDATE test_table SET c = 2 WHERE a = 1");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });

      statement.execute("REVOKE SELECT ON TABLE test_table FROM test_role");
      withRole(statement, "test_role", () -> {
        // Can no longer perform updates with selection.
        runInvalidQuery(
            statement,
            "UPDATE test_table SET c = 2 WHERE a = 1",
            PERMISSION_DENIED
        );

        // Can still perform simple updates.
        statement.execute("UPDATE test_table SET c = 2");
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * ALL
       */

      statement.execute("GRANT ALL ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // All are allowed.
        statement.execute("SELECT * FROM test_table");
        statement.execute("INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)");
        statement.execute("UPDATE test_table SET c = 2");
        statement.execute("DELETE FROM test_table WHERE a = 2");
      });
    }
  }

  @Test
  public void testTemporaryTableColumnPrivileges() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      statement.execute("CREATE TEMP TABLE test_table(a int, b int, c int, PRIMARY KEY (a))");

      /*
       * SELECT
       */

      statement.execute("GRANT SELECT(a) ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Select from "a" is allowed.
        statement.execute("SELECT a FROM test_table");

        // Others are not.
        runInvalidQuery(statement, "SELECT b FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT c FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      statement.execute("GRANT SELECT(a, b, c) ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Select is allowed.
        statement.execute("SELECT * FROM test_table");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });

      // Revoke a single column.
      statement.execute("REVOKE SELECT(b) ON TABLE test_table FROM test_role");
      withRole(statement, "test_role", () -> {
        // Select from "a" and "c" are allowed.
        statement.execute("SELECT a FROM test_table");
        statement.execute("SELECT c FROM test_table");
        statement.execute("SELECT a, c FROM test_table");

        // Others are not.
        runInvalidQuery(statement, "SELECT b FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * INSERT
       */

      statement.execute("GRANT INSERT(a) ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Insert "a" is allowed.
        statement.execute("INSERT INTO test_table(a) VALUES (2)");

        // Others are not.
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(b) VALUES (4)",
            PERMISSION_DENIED
        );
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * UPDATE
       */

      statement.execute("GRANT UPDATE(c) ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Update is allowed.
        statement.execute("UPDATE test_table SET c = 2");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "UPDATE test_table SET c = 2 WHERE a = 1",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "UPDATE test_table SET a = 2, b = 2, c = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "UPDATE test_table SET b = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * UPDATE, SELECT
       */

      statement.execute("GRANT UPDATE(c), SELECT(a) ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // Update with selection is allowed.
        statement.execute("UPDATE test_table SET c = 2 WHERE a = 1");

        // Others are not allowed.
        runInvalidQuery(
            statement,
            "UPDATE test_table SET a = 2, b = 2, c = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "UPDATE test_table SET b = 2",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(c) VALUES (2)",
            PERMISSION_DENIED
        );
      });
      statement.execute("REVOKE ALL ON TABLE test_table FROM test_role");

      /*
       * ALL
       */

      statement.execute("GRANT ALL(a, b) ON TABLE test_table TO test_role");
      withRole(statement, "test_role", () -> {
        // All on "a" and "b" are allowed.
        statement.execute("SELECT a FROM test_table");
        statement.execute("INSERT INTO test_table(a, b) VALUES (3, 4)");
        statement.execute("UPDATE test_table SET a = 2 WHERE b = 3");

        // Others are not.
        runInvalidQuery(statement, "SELECT b, c FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(
            statement,
            "INSERT INTO test_table(a, b, c) VALUES (1, 2, 3)",
            PERMISSION_DENIED
        );
        runInvalidQuery(statement, "UPDATE test_table SET c = 3", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testRevokeAdminOption() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE some_role");
      statement.execute("CREATE ROLE some_group ADMIN some_role");

      statement.execute("CREATE ROLE other_role");

      withRole(statement, "some_role", () -> {
        // "some_role" has admin option in "some_group".
        statement.execute("GRANT some_group TO other_role");
      });

      statement.execute("REVOKE ADMIN OPTION FOR some_group FROM some_role");

      withRole(statement, "some_role", () -> {
        // "some_role" no longer has admin option in "some_group".
        runInvalidQuery(
            statement,
            "GRANT some_group TO other_role",
            "must have admin option on role \"some_group\""
        );

        // "some_role" is still a member of "some_group".
        statement.execute("SET ROLE some_group");
      });

      withRole(statement, "other_role", () -> {
        // "other_role" is still a member of "some_group".
        statement.execute("SET ROLE some_group");
      });
    }
  }

  @Test
  public void testCascadingRevoke() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE a");
      statement.execute("CREATE ROLE b");

      // Create table and grant privileges to role "a".
      statement.execute("CREATE TABLE test_table(id int)");
      statement.execute("GRANT ALL ON TABLE test_table TO a WITH GRANT OPTION");

      withRole(statement, "a", () -> {
        // Grant table to "b" from "a".
        statement.execute("GRANT ALL ON TABLE test_table TO b");
      });

      withRole(statement, "b", () -> {
        // Role "b" has indirectly received privileges for "test_table".
        statement.execute("SELECT * FROM test_table");
      });

      // Revoking privileges from "a" without cascading results in an error.
      runInvalidQuery(
          statement,
          "REVOKE ALL ON TABLE test_table FROM a",
          "dependent privileges exist"
      );

      // Revoke privileges from "a" and cascade to any privileges granted by "a".
      statement.execute("REVOKE ALL ON TABLE test_table FROM a CASCADE");

      withRole(statement, "a", () -> {
        // Role "a" no longer has privileges.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
      });

      withRole(statement, "b", () -> {
        // Role "b" has also lost its privileges.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testReferencesOnTables() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE has_references");
      statement.execute("CREATE ROLE no_references");

      statement.execute("CREATE TABLE customers(id int PRIMARY KEY)");
      statement.execute("GRANT REFERENCES ON TABLE customers TO has_references");

      withRole(statement, "has_references", () -> {
        // "has_references" can create a table referencing a column in "customers".
        statement.execute(
            "CREATE TABLE orders(id int PRIMARY KEY, cid int REFERENCES customers(id))");
      });

      withRole(statement, "no_references", () -> {
        // "no_references" user cannot create a table referencing a column in "customers".
        runInvalidQuery(
            statement,
            "CREATE TABLE emails(id int PRIMARY KEY, cid int REFERENCES customers(id))",
            PERMISSION_DENIED
        );
      });
    }
  }

  @Test
  public void testReferencesOnColumns() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE has_references");
      statement.execute("CREATE ROLE no_references");

      statement.execute("CREATE TABLE table1(id1 int PRIMARY KEY, id2 int UNIQUE)");
      statement.execute("GRANT REFERENCES(id2) ON TABLE table1 TO has_references");

      statement.execute("CREATE TABLE table2(id1 int PRIMARY KEY, id2 int UNIQUE, id3 int UNIQUE)");
      statement.execute("GRANT REFERENCES(id1, id3) ON TABLE table2 TO has_references");

      withRole(statement, "has_references", () -> {
        // Can reference single granted column from table1.
        statement.execute(
            "CREATE TABLE test_table(id int PRIMARY KEY, cid int REFERENCES table1(id2))");
        statement.execute("DROP TABLE test_table");

        // Can reference single granted column from table2.
        statement.execute(
            "CREATE TABLE test_table(id int PRIMARY KEY, cid int REFERENCES table2(id1))");
        statement.execute("DROP TABLE test_table");

        // Can reference multiple granted columns from table2.
        statement.execute("CREATE TABLE test_table(id int PRIMARY KEY," +
            " cid int REFERENCES table2(id1)," +
            " did int REFERENCES table2(id3))");
        statement.execute("DROP TABLE test_table");

        // Cannot reference columns which were not granted.
        runInvalidQuery(
            statement,
            "CREATE TABLE test_table(id int PRIMARY KEY, cid int REFERENCES table1(id1))",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "CREATE TABLE test_table(id int PRIMARY KEY," +
                " cid int REFERENCES table1(id1)," +
                " did int REFERENCES table1(id2))",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "CREATE TABLE test_table(id int PRIMARY KEY," +
                " cid int REFERENCES table2(id3)," +
                " did int REFERENCES table2(id2))",
            PERMISSION_DENIED
        );
      });

      withRole(statement, "no_references", () -> {
        // "no_references" role cannot create tables referencing columns in either table.
        runInvalidQuery(
            statement,
            "CREATE TABLE test_table(id int PRIMARY KEY, cid int REFERENCES table1(id2))",
            PERMISSION_DENIED
        );
        runInvalidQuery(
            statement,
            "CREATE TABLE test_table(id int PRIMARY KEY, cid int REFERENCES table2(id3))",
            PERMISSION_DENIED
        );
      });
    }
  }

  @Test
  public void testForeignKeyConstraintACLCheck() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      statement.execute("CREATE TABLE test_table(id1 int PRIMARY KEY, id2 int UNIQUE)");
      statement.execute("GRANT REFERENCES(id1), SELECT ON TABLE test_table TO test_role");

      withRole(statement, "test_role", () -> {
        statement.execute(
            "CREATE TABLE reference_table(id int PRIMARY KEY, cid int)");

        runInvalidQuery(
            statement,
            "ALTER TABLE reference_table ADD FOREIGN KEY (cid) REFERENCES test_table(id2)",
            PERMISSION_DENIED
        );

        statement.execute(
            "ALTER TABLE reference_table ADD FOREIGN KEY (cid) REFERENCES test_table(id1)");
      });
    }
  }

  @Test
  public void testOnAllTablesInSchema() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      // Create table outside schema.
      statement.execute("CREATE TABLE test1(id int)");

      statement.execute("CREATE SCHEMA ts");
      statement.execute("GRANT USAGE ON SCHEMA ts TO test_role");

      // Create tables inside schema.
      statement.execute("CREATE TABLE ts.test1(id int)");
      statement.execute("CREATE TABLE ts.test2(id int)");

      statement.execute("GRANT SELECT ON ALL TABLES IN SCHEMA ts TO test_role");

      // Create table inside schema, after grant has occurred.
      statement.execute("CREATE TABLE ts.test3(id int)");

      withRole(statement, "test_role", () -> {
        // Only tables which were in the schema before the grant have privileges.
        runInvalidQuery(statement, "SELECT * FROM test1", PERMISSION_DENIED);
        statement.execute("SELECT * FROM ts.test1");
        statement.execute("SELECT * FROM ts.test2");
        runInvalidQuery(statement, "SELECT * FROM ts.test3", PERMISSION_DENIED);

        // Tables only have select privileges.
        runInvalidQuery(statement, "TRUNCATE ts.test1", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testPrivilegesAsSumOfGrantAcrossMembership() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");
      statement.execute("CREATE ROLE test_group ROLE test_role");

      // Create function.
      statement.execute("CREATE FUNCTION test() RETURNS int AS 'SELECT 1' LANGUAGE SQL");

      // Grant EXECUTE to "test_role", and both groups it is a member of.
      statement.execute("GRANT ALL ON FUNCTION test() TO test_role");
      statement.execute("GRANT ALL ON FUNCTION test() TO test_group");
      statement.execute("GRANT ALL ON FUNCTION test() TO PUBLIC");

      // Revoking from "test_role" does not change sum of grant privileges.
      statement.execute("REVOKE ALL ON FUNCTION test() FROM test_role");
      withRole(statement, "test_role", () -> {
        // Function can still be executed.
        statement.execute("SELECT test()");
      });

      // Revoking from PUBLIC does not change sum of grant privileges.
      statement.execute("REVOKE ALL ON FUNCTION test() FROM PUBLIC");
      withRole(statement, "test_role", () -> {
        // Function can still be executed.
        statement.execute("SELECT test()");
      });

      // Revoking from "test_group" removes all granted privileges.
      statement.execute("REVOKE ALL ON FUNCTION test() FROM test_group");
      withRole(statement, "test_role", () -> {
        // Function can no longer be executed.
        runInvalidQuery(statement, "SELECT test()", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testOnAllFunctionsInSchema() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role");

      // By default, don't allow public to execute new functions.
      statement.execute(
          "ALTER DEFAULT PRIVILEGES REVOKE EXECUTE ON FUNCTIONS FROM PUBLIC");

      // Create function outside schema.
      statement.execute("CREATE FUNCTION test1() RETURNS int AS 'SELECT 1' LANGUAGE SQL");

      statement.execute("CREATE SCHEMA ts");
      statement.execute("GRANT USAGE ON SCHEMA ts TO test_role");

      // Create functions inside schema.
      statement.execute("CREATE FUNCTION ts.test1() RETURNS int AS 'SELECT 1' LANGUAGE SQL");
      statement.execute("CREATE FUNCTION ts.test2() RETURNS int AS 'SELECT 1' LANGUAGE SQL");

      statement.execute("GRANT EXECUTE ON ALL FUNCTIONS IN SCHEMA ts TO test_role");

      // Create function inside schema, after grant has occurred.
      statement.execute("CREATE FUNCTION ts.test3() RETURNS int AS 'SELECT 1' LANGUAGE SQL");

      withRole(statement, "test_role", () -> {
        // Only functions which were in the schema before the grant have privileges.
        runInvalidQuery(statement, "SELECT test1()", PERMISSION_DENIED);
        statement.execute("SELECT ts.test1()");
        statement.execute("SELECT ts.test2()");
        runInvalidQuery(statement, "SELECT ts.test3()", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testAlterObjectOwner() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE old_user");
      statement.execute("CREATE ROLE new_user");
      statement.execute("CREATE ROLE some_group ROLE old_user");

      // Create a schema and grant access.
      statement.execute("CREATE SCHEMA ts");
      statement.execute("GRANT ALL ON SCHEMA ts TO old_user, new_user");

      withRole(statement, "old_user", () -> {
        // Create some tables.
        statement.execute("CREATE TABLE test_table(id int)");
        statement.execute("CREATE TABLE ts.test_table(id int)");

        // Transfer ownership of table to parent.
        statement.execute("ALTER TABLE test_table OWNER TO some_group");

        // Can still select, since "old_user" is a member of "some_group" (the new owner).
        statement.execute("SELECT * FROM test_table");

        // Can still perform owner-only operations, since "old_user" is a member of "some_group".
        statement.execute("DROP TABLE test_table");

        // Cannot assign ownership to a role we are not a member of.
        runInvalidQuery(
            statement,
            "ALTER TABLE ts.test_table OWNER TO new_user",
            "must be member of role \"new_user\""
        );
      });

      // Superusers can assign ownership to any role.
      statement.execute("ALTER TABLE ts.test_table OWNER TO new_user");

      // This should be a no-op.
      statement.execute("DROP OWNED BY old_user CASCADE");

      withRole(statement, "old_user", () -> {
        // Old user has lost all privileges on the table.
        runInvalidQuery(statement, "SELECT * FROM ts.test_table", PERMISSION_DENIED);
      });

      withRole(statement, "new_user", () -> {
        // New role has ownership privileges on the table.
        statement.execute("DROP TABLE ts.test_table");
      });
    }
  }

  @Test
  public void testAlterDatabaseOwner() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE su1 CREATEDB");
      statement.execute("CREATE ROLE su2 LOGIN CREATEDB");

      statement.execute("GRANT su1 TO su2");

      withRole(statement, "su1", () -> {
        // Create a database owned by "su1".
        statement.execute("CREATE DATABASE su1_db");
      });

      // Alter database owner externally.
      statement.execute("ALTER DATABASE su1_db OWNER TO su2");

      withRole(statement, "su2", () -> {
        // Create a database owned by "su2".
        statement.execute("CREATE DATABASE su2_db");
      });
    }

    try (Connection connection = getConnectionBuilder().withDatabase("su2_db")
        .withUser("su2").connect();
         Statement statement = connection.createStatement()) {
      // Alter database owner from within database.
      statement.execute("ALTER DATABASE su2_db OWNER TO su1");
    }

    try (Statement statement = connection.createStatement()) {
      // Allow time for cache to refresh after ALTER above.
      waitForTServerHeartbeat();

      statement.execute("REVOKE su1 FROM su2");

      // Database owners are correctly set.
      withRole(statement, "su1", () ->
          runInvalidQuery(statement, "DROP DATABASE su1_db", "must be owner of database su1_db"));
      withRole(statement, "su2", () ->
          runInvalidQuery(statement, "DROP DATABASE su2_db", "must be owner of database su2_db"));
      withRole(statement, "su1", () ->
          statement.execute("DROP DATABASE su2_db"));
      withRole(statement, "su2", () ->
          statement.execute("DROP DATABASE su1_db"));
    }
  }

  @Test
  public void testReassignOwnedObjects() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE old_user");
      statement.execute("CREATE ROLE new_user");
      statement.execute("CREATE ROLE some_group ROLE old_user");

      // Create a schema and grant access.
      statement.execute("CREATE SCHEMA ts");
      statement.execute("GRANT ALL ON SCHEMA ts TO old_user, new_user");

      withRole(statement, "old_user", () -> {
        // Create some objects.
        statement.execute("CREATE TABLE test_table(id int PRIMARY KEY)");
        statement.execute("CREATE FUNCTION ts.test() RETURNS int AS 'SELECT 1' LANGUAGE SQL");
        statement.execute("CREATE TEMP TABLE temp_table(id int)");
        statement.execute("CREATE VIEW ts.test_view AS SELECT * FROM public.test_table");
        statement.execute(
            "CREATE TABLE referencing_table(id int, tid int REFERENCES test_table(id))");
        statement.execute(
            "ALTER DEFAULT PRIVILEGES IN SCHEMA ts GRANT EXECUTE ON FUNCTIONS TO PUBLIC");

        // Cannot reassign ownership to roles we are not a member of.
        runInvalidQuery(
            statement,
            "REASSIGN OWNED BY CURRENT_USER TO new_user",
            PERMISSION_DENIED
        );

        // Reassign fails because "some_group" does not have schema permissions.
        runInvalidQuery(
            statement,
            "REASSIGN OWNED BY CURRENT_USER TO some_group",
            "permission denied for schema ts"
        );

        withRole(statement, TEST_PG_USER, () ->
            statement.execute("GRANT ALL ON SCHEMA ts TO some_group"));

        // Can reassign ownership after granting.
        statement.execute("REASSIGN OWNED BY CURRENT_USER TO some_group");

        // Old role still has ownership privileges, since it is a member of "some_group".
        statement.execute("SELECT * FROM test_table");
        statement.execute("SELECT * FROM ts.test_view");
        statement.execute("ALTER TABLE test_table ADD COLUMN c1 int");

        // Create new object as old_user.
        statement.execute("CREATE TABLE new_table(id int)");
      });

      withRole(statement, "some_group", () -> {
        // Group has ownership privileges on original objects.
        statement.execute("SELECT * FROM test_table");
        statement.execute("SELECT * FROM ts.test_view");
        statement.execute("ALTER TABLE test_table ADD COLUMN c2 int");

        // Group does not have privileges on new table.
        runInvalidQuery(statement, "SELECT * FROM new_table", PERMISSION_DENIED);
      });

      withRole(statement, "new_user", () -> {
        // New user still does not have permissions on objects.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM new_table", PERMISSION_DENIED);
      });

      // Reassign ownership of both roles to the new user.
      statement.execute("REASSIGN OWNED BY some_group, old_user TO new_user");

      // This should be a no-op.
      statement.execute("DROP OWNED BY some_group, old_user CASCADE");

      withRole(statement, "new_user", () -> {
        // New user has permissions on original objects and the new table.
        statement.execute("SELECT * FROM test_table");
        statement.execute("SELECT * FROM ts.test_view");
        statement.execute("ALTER TABLE test_table ADD COLUMN c3 int");
        statement.execute("SELECT * FROM new_table");
      });

      withRole(statement, "old_user", () -> {
        // Old user no longer has permissions.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM new_table", PERMISSION_DENIED);
      });

      withRole(statement, "some_group", () -> {
        // Group no longer has permissions.
        runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement, "SELECT * FROM new_table", PERMISSION_DENIED);
      });

      // REASSIGN does not remove permissions for unowned objects.
      statement.execute("GRANT SELECT ON new_table TO old_user");
      statement.execute("REASSIGN OWNED BY old_user TO new_user");
      runInvalidQuery(
          statement,
          "DROP ROLE old_user",
          "role \"old_user\" cannot be dropped because some objects depend on it"
      );
    }
  }

  @Test
  public void testDropOwnershipOnDatabaseObjects() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE test_role SUPERUSER");

      /*
       * SIMPLE
       */

      withRole(statement, "test_role", () -> {
        statement.execute("CREATE TABLE test_table(id int)");

        // This should drop the created table.
        statement.execute("DROP OWNED BY test_role");

        statement.execute("CREATE TABLE test_table(id int)");
      });

      // DROP OWNED does remove permissions for unowned objects.
      statement.execute("CREATE TABLE new_table(id int)");
      statement.execute("GRANT SELECT ON TABLE new_table TO test_role");
      statement.execute("DROP OWNED BY test_role");
      statement.execute("DROP ROLE test_role");

      statement.execute("CREATE ROLE test_role SUPERUSER");

      /*
       * CASCADE/RESTRICT
       */

      withRole(statement, "test_role", () -> {
        statement.execute("CREATE ROLE schema_user");

        statement.execute("CREATE SCHEMA test_schema");
        statement.execute("GRANT ALL ON SCHEMA test_schema TO schema_user");

        withRole(statement, "schema_user", () -> {
          // Create a table in "test_schema" which is not owned by "test_role".
          statement.execute("CREATE TABLE test_schema.test_table(id int)");
        });

        // DROP and DROP RESTRICT fail due to unowned table.
        runInvalidQuery(
            statement,
            "DROP OWNED BY test_role RESTRICT",
            "table test_schema.test_table depends on schema test_schema"
        );
        runInvalidQuery(
            statement,
            "DROP OWNED BY test_role",
            "table test_schema.test_table depends on schema test_schema"
        );

        // DROP CASCADE succeeds.
        statement.execute("DROP OWNED BY test_role CASCADE");

        // Schema and table were dropped.
        statement.execute("CREATE SCHEMA test_schema");
        statement.execute("CREATE TABLE test_schema.test_table(id int)");
      });
    }
  }

  @Test
  public void testMultiDatabaseOwnershipDrop() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE USER su LOGIN SUPERUSER");
      withRole(statement, "su", () -> {
        // Create a database owned by "su".
        statement.execute("CREATE DATABASE su_db");

        // Create object in default database.
        statement.execute("CREATE TABLE test_table(id int)");
      });
    }

    try (Connection connection = getConnectionBuilder().withDatabase("su_db")
        .withUser("su").connect();
         Statement statement = connection.createStatement()) {
      // Create object in "su_db".
      statement.execute("CREATE TABLE test_table(id int)");

      statement.execute("DROP OWNED BY su CASCADE");

      runInvalidQuery(statement, "SELECT * FROM test_table", "does not exist");
    }

    try (Statement statement = connection.createStatement()) {
      // Objects outside "su_db" owned by "su" were not dropped.
      statement.execute("SELECT * FROM test_table");

      statement.execute("DROP OWNED BY su CASCADE");

      runInvalidQuery(statement, "SELECT * FROM test_table", "does not exist");
    }

    // Database was not dropped, so we can reconnect.
    try (Connection ignored = getConnectionBuilder().withDatabase("su_db")
        .withUser("su").connect()) {
      // No-op.
    }
  }

  @Test
  public void testMultiDatabaseOwnershipReassign() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE su LOGIN SUPERUSER");
      statement.execute("CREATE ROLE recipient");

      withRole(statement, "su", () -> {
        // Create a database owned by "su".
        statement.execute("CREATE DATABASE su_db");

        // Create object in default database.
        statement.execute("CREATE TABLE test_table(id int)");
      });
    }

    try (Connection connection = getConnectionBuilder().withDatabase("su_db")
        .withUser("su").connect();
         Statement statement = connection.createStatement()) {
      // Create object in "su_db".
      statement.execute("CREATE TABLE test_table(id int)");

      statement.execute("REASSIGN OWNED BY su TO recipient");

      withRole(statement, "recipient", () ->
          statement.execute("SELECT * FROM test_table"));
    }

    try (Statement statement = connection.createStatement()) {
      // Objects outside "su_db" owned by "su" were not reassigned.
      withRole(statement, "recipient", () ->
          runInvalidQuery(statement, "SELECT * FROM test_table", PERMISSION_DENIED));

      statement.execute("REASSIGN OWNED BY su TO recipient");

      withRole(statement, "recipient", () ->
          statement.execute("SELECT * FROM test_table"));
    }
  }

  @Test
  public void testCreateSchemaAuthorization() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE ROLE unprivileged");
      statement.execute("CREATE ROLE owner_role");

      // Create schema and make "owner_role" the owner.
      statement.execute("CREATE SCHEMA test_schema AUTHORIZATION owner_role");

      withRole(statement, "owner_role", () -> {
        // Owner has CREATE and USAGE privileges.
        statement.execute("CREATE TABLE test_schema.test_table(id int)");
        statement.execute("SELECT * FROM test_schema.test_table");
        statement.execute("DROP TABLE test_schema.test_table");
      });

      withRole(statement, "unprivileged", () -> {
        // Other users have no privileges.
        runInvalidQuery(
            statement,
            "CREATE TABLE test_schema.test_table2(id int)",
            PERMISSION_DENIED
        );
        runInvalidQuery(statement, "SELECT * FROM test_schema.test_table", PERMISSION_DENIED);
      });

      withRole(statement, "owner_role", () -> {
        // Owner has DROP and GRANT privileges (owner-only privileges).
        statement.execute("GRANT USAGE ON SCHEMA test_schema TO unprivileged");
        statement.execute("DROP SCHEMA test_schema");
      });
    }
  }

  @Test
  public void testRevokeLoginMidSession() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {

      statement1.execute("CREATE ROLE test_role LOGIN");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        // Revoke login privileges now that "test_role" has connected.
        statement1.execute("ALTER ROLE test_role NOLOGIN");

        waitForTServerHeartbeat();

        // Open session is still usable, even after the alter.
        statement2.execute("SELECT count(*) FROM pg_class");
      }

      // Next login attempt fails.
      try (Connection ignored = getConnectionBuilder().withUser("test_role").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("role \"test_role\" is not permitted to log in")
        );
      }
    }
  }

  @Test
  public void testGrantAttributesMidSession() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {

      statement1.execute("CREATE ROLE test_role LOGIN");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        runInvalidQuery(statement2, "CREATE ROLE tr", PERMISSION_DENIED);
        runInvalidQuery(statement2, "CREATE DATABASE tdb", PERMISSION_DENIED);
        runInvalidQuery(statement2, "CREATE ROLE su SUPERUSER", "must be superuser");

        // Grant CREATEROLE from connection 1.
        statement1.execute("ALTER ROLE test_role CREATEROLE");

        waitForTServerHeartbeat();

        // New attribute observed on connection 2 after heartbeat.
        statement2.execute("CREATE ROLE tr");
        runInvalidQuery(statement2, "CREATE DATABASE tdb", PERMISSION_DENIED);
        runInvalidQuery(statement2, "CREATE ROLE su SUPERUSER", "must be superuser");

        // Grant CREATEDB from connection 1.
        statement1.execute("ALTER ROLE test_role CREATEDB");

        waitForTServerHeartbeat();

        // New attribute observed on connection 2 after heartbeat.
        statement2.execute("CREATE DATABASE tdb");
        runInvalidQuery(statement2, "CREATE ROLE su SUPERUSER", "must be superuser");

        // Grant SUPERUSER from connection 1.
        statement1.execute("ALTER ROLE test_role SUPERUSER");

        waitForTServerHeartbeat();

        // New attribute observed on connection 2 after heartbeat.
        statement2.execute("CREATE ROLE su SUPERUSER");

        // "test_role" still cannot set their session authorization, despite having
        // superuser privileges.
        runInvalidQuery(statement2, "SET SESSION AUTHORIZATION su", PERMISSION_DENIED);
      }
    }
  }

  @Test
  public void testRevokeAttributesMidSession() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {

      statement1.execute("CREATE ROLE test_role LOGIN CREATEROLE CREATEDB SUPERUSER");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        statement2.execute("CREATE ROLE su SUPERUSER");

        // Revoke SUPERUSER from connection 1.
        statement1.execute("ALTER ROLE test_role NOSUPERUSER");

        waitForTServerHeartbeat();

        // Lost attribute is observed from connection 2.
        runInvalidQuery(statement2, "CREATE ROLE su SUPERUSER", "must be superuser");
        statement2.execute("CREATE DATABASE tdb");

        // Revoke CREATEDB from connection 1.
        statement1.execute("ALTER ROLE test_role NOCREATEDB");

        waitForTServerHeartbeat();

        // Lost attribute is observed from connection 2.
        runInvalidQuery(statement2, "CREATE ROLE su SUPERUSER", "must be superuser");
        runInvalidQuery(statement2, "CREATE DATABASE tdb", PERMISSION_DENIED);
        statement2.execute("CREATE ROLE tr");

        // Revoke CREATEROLE from connection 1.
        statement1.execute("ALTER ROLE test_role NOCREATEROLE");

        waitForTServerHeartbeat();

        // Lost attribute is observed from connection 2.
        runInvalidQuery(statement2, "CREATE ROLE su SUPERUSER", "must be superuser");
        runInvalidQuery(statement2, "CREATE DATABASE tdb", PERMISSION_DENIED);
        runInvalidQuery(statement2, "CREATE ROLE tr", PERMISSION_DENIED);
      }
    }
  }

  @Test
  public void testConnectionLimitDecreasedMidSession() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {

      statement1.execute("CREATE ROLE test_role LOGIN CONNECTION LIMIT 1");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        // Decrease connection limit now that "test_role" has connected.
        statement1.execute("ALTER ROLE test_role CONNECTION LIMIT 0");

        waitForTServerHeartbeat();

        // Open session is still usable, even after the alter.
        statement2.execute("SELECT count(*) FROM pg_class");
      }

      // Next login attempt fails.
      try (Connection ignored = getConnectionBuilder().withUser("test_role").connect()) {
        fail("Expected login attempt to fail");
      } catch (SQLException sqle) {
        assertThat(
            sqle.getMessage(),
            CoreMatchers.containsString("too many connections for role \"test_role\"")
        );
      }
    }
  }

  @Test
  public void testParentAttributeChangedMidSession() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {
      statement1.execute("CREATE ROLE test_role INHERIT LOGIN");
      statement1.execute("CREATE ROLE test_group INHERIT ROLE test_role");

      // Create group with some privileges.
      statement1.execute("CREATE ROLE test_large_group ROLE test_group");
      statement1.execute("CREATE TABLE test_table(id int)");
      statement1.execute("GRANT SELECT ON test_table TO test_large_group");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        // Connection 2 initially has select privileges.
        statement2.execute("SELECT * FROM test_table");

        // Disable parent inheritance from connection 1.
        statement1.execute("ALTER ROLE test_group NOINHERIT");

        waitForTServerHeartbeat();

        // Connection 2 has lost inherited privileges.
        runInvalidQuery(statement2, "SELECT * FROM test_table", PERMISSION_DENIED);

        // Enable parent inheritance from connection 1.
        statement1.execute("ALTER ROLE test_group INHERIT");

        waitForTServerHeartbeat();

        // Select privileges restored to connection 2.
        statement2.execute("SELECT * FROM test_table");
      }
    }
  }

  @Test
  public void testMembershipRevokedInsideGroup() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {
      statement1.execute("CREATE ROLE test_role LOGIN");

      // Create group with some privileges.
      statement1.execute("CREATE ROLE test_group ROLE test_role");
      statement1.execute("CREATE TABLE test_table(id int)");
      statement1.execute("GRANT SELECT ON test_table to test_group");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        statement2.execute("SET ROLE test_group");

        // Can initially use group privileges.
        assertQuery(statement2, "SELECT * FROM test_table");

        // Revoke membership on another node.
        statement1.execute("REVOKE test_group FROM test_role");

        waitForTServerHeartbeat();

        // Can still use group privileges, even though membership has been revoked.
        assertQuery(statement2, "SELECT * FROM test_table");

        // Cannot set role to the group anymore.
        runInvalidQuery(statement2, "SET ROLE test_group", PERMISSION_DENIED);
        statement2.execute("RESET ROLE");
        runInvalidQuery(statement2, "SET ROLE test_group", PERMISSION_DENIED);
      }
    }
  }

  @Test
  public void testMembershipRevokedOutsideGroup() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {
      statement1.execute("CREATE ROLE test_role LOGIN INHERIT");

      // Create group with some privileges.
      statement1.execute("CREATE ROLE test_group ROLE test_role");
      statement1.execute("CREATE TABLE test_table(id int)");
      statement1.execute("GRANT SELECT ON test_table to test_group");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        // Can initially use group privileges.
        assertQuery(statement2, "SELECT * FROM test_table");

        // Revoke membership on another node.
        statement1.execute("REVOKE test_group FROM test_role");

        waitForTServerHeartbeat();

        // Immediately lose group privileges.
        runInvalidQuery(statement2, "SELECT * FROM test_table", PERMISSION_DENIED);

        // Cannot set role to the group anymore.
        runInvalidQuery(statement2, "SET ROLE test_group", PERMISSION_DENIED);
      }
    }
  }

  @Test
  public void testInheritanceSwitchedWhileInsideGroup() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {
      statement1.execute("CREATE ROLE test_role LOGIN INHERIT");

      // Create group with some privileges.
      statement1.execute("CREATE ROLE test_group ROLE test_role");
      statement1.execute("CREATE TABLE test_table(id int)");
      statement1.execute("GRANT SELECT ON test_table to test_group");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        // Connection 2 initially has select privileges.
        statement2.execute("SELECT * FROM test_table");

        // Remove inheritance from connection 1.
        statement1.execute("ALTER ROLE test_role NOINHERIT");

        waitForTServerHeartbeat();

        // Connection 2 has lost inherited privileges.
        runInvalidQuery(statement2, "SELECT * FROM test_table", PERMISSION_DENIED);

        // Add inheritance from connection 1.
        statement1.execute("ALTER ROLE test_role INHERIT");

        waitForTServerHeartbeat();

        // Select privileges restored to connection 2.
        statement2.execute("SELECT * FROM test_table");
      }
    }
  }

  @Test
  public void testRoleRenamingMidSession() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Statement statement1 = connection1.createStatement()) {

      statement1.execute("CREATE ROLE test_role LOGIN");
      statement1.execute("CREATE GROUP test_group ROLE test_role");

      try (Connection connection2 = getConnectionBuilder().withTServer(1)
          .withUser("test_role").connect();
           Statement statement2 = connection2.createStatement()) {
        statement2.execute("SET ROLE test_group");

        assertEquals("test_role", getSessionUser(statement2));
        assertEquals("test_group", getCurrentUser(statement2));

        // Rename role from connection 1.
        statement1.execute("ALTER ROLE test_group RENAME TO test_group1");

        waitForTServerHeartbeat();

        // New name is visible from connection 2.
        assertEquals("test_role", getSessionUser(statement2));
        assertEquals("test_group1", getCurrentUser(statement2));

        // Rename role from connection 1.
        statement1.execute("ALTER ROLE test_role RENAME TO test_role1");

        waitForTServerHeartbeat();

        // New name is visible from connection 2.
        assertEquals("test_role1", getSessionUser(statement2));
        assertEquals("test_group1", getCurrentUser(statement2));
      }
    }
  }

  @Test
  public void testMultiNodePermissionChanges() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Connection connection2 = getConnectionBuilder().withTServer(1).connect();
         Statement statement1 = connection1.createStatement();
         Statement statement2 = connection2.createStatement()) {
      statement1.execute("CREATE TABLE test_table(id int)");

      statement2.execute("CREATE ROLE test_role");

      withRole(statement2, "test_role", () -> {
        // No permissions by default for table.
        runInvalidQuery(statement2, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement2, "INSERT INTO test_table VALUES (1)", PERMISSION_DENIED);

        // Grant SELECT from connection 1.
        statement1.execute("GRANT SELECT ON TABLE test_table TO test_role");

        waitForTServerHeartbeat();

        statement2.execute("SELECT * FROM test_table");
        runInvalidQuery(statement2, "INSERT INTO test_table VALUES (1)", PERMISSION_DENIED);

        // Grant INSERT from connection 1.
        statement1.execute("GRANT INSERT ON TABLE test_table TO test_role");

        waitForTServerHeartbeat();

        statement2.execute("SELECT * FROM test_table");
        statement2.execute("INSERT INTO test_table VALUES (1)");

        // Revoke all from connection 1.
        statement1.execute("REVOKE ALL ON TABLE test_table FROM test_role");

        waitForTServerHeartbeat();

        runInvalidQuery(statement2, "SELECT * FROM test_table", PERMISSION_DENIED);
        runInvalidQuery(statement2, "INSERT INTO test_table VALUES (1)", PERMISSION_DENIED);
      });
    }
  }

  @Test
  public void testAlterDefaultPrivilegesAcrossNodes() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Connection connection2 = getConnectionBuilder().withTServer(1).connect();
         Statement statement1 = connection1.createStatement();
         Statement statement2 = connection2.createStatement()) {
      statement1.execute("CREATE SCHEMA ts");
      statement1.execute("GRANT USAGE ON SCHEMA ts TO PUBLIC");
      statement1.execute("CREATE ROLE test_role LOGIN");
      statement2.execute("CREATE TABLE ts.table1(id int)");

      try (Connection connection3 = getConnectionBuilder().withTServer(2)
          .withUser("test_role").connect();
           Statement statement3 = connection3.createStatement()) {
        runInvalidQuery(statement3, "SELECT * FROM ts.table1", PERMISSION_DENIED);

        // Grant select by default from connection 1.
        statement1.execute(
            "ALTER DEFAULT PRIVILEGES IN SCHEMA ts GRANT SELECT ON TABLES TO PUBLIC");
        waitForTServerHeartbeat();
        statement2.execute("CREATE TABLE ts.table2(id int)");

        // New table has select privileges, old tables unaffected.
        runInvalidQuery(statement3, "SELECT * FROM ts.table1", PERMISSION_DENIED);
        statement3.execute("SELECT * FROM ts.table2");
        runInvalidQuery(statement3, "INSERT INTO ts.table2(id) VALUES (1)", PERMISSION_DENIED);

        // Grant insert by default from connection 1.
        statement1.execute(
            "ALTER DEFAULT PRIVILEGES IN SCHEMA ts GRANT INSERT ON TABLES TO PUBLIC");
        waitForTServerHeartbeat();
        statement2.execute("CREATE TABLE ts.table3(id int)");

        // New table has select and insert privileges, old tables unaffected.
        runInvalidQuery(statement3, "SELECT * FROM ts.table1", PERMISSION_DENIED);
        statement3.execute("SELECT * FROM ts.table2");
        runInvalidQuery(statement3, "INSERT INTO ts.table2(id) VALUES (1)", PERMISSION_DENIED);
        statement3.execute("SELECT * FROM ts.table3");
        statement3.execute("INSERT INTO ts.table3(id) VALUES (1)");

        // Revoke all privileges by default from connection 1.
        statement1.execute(
            "ALTER DEFAULT PRIVILEGES IN SCHEMA ts REVOKE ALL ON TABLES FROM PUBLIC");
        waitForTServerHeartbeat();
        statement2.execute("CREATE TABLE ts.table4(id int)");

        // New table has no privileges, old tables unaffected.
        runInvalidQuery(statement3, "SELECT * FROM ts.table1", PERMISSION_DENIED);
        statement3.execute("SELECT * FROM ts.table2");
        runInvalidQuery(statement3, "INSERT INTO ts.table2(id) VALUES (1)", PERMISSION_DENIED);
        statement3.execute("SELECT * FROM ts.table3");
        statement3.execute("INSERT INTO ts.table3(id) VALUES (1)");
        runInvalidQuery(statement3, "SELECT * FROM ts.table4", PERMISSION_DENIED);
        runInvalidQuery(statement3, "INSERT INTO ts.table4(id) VALUES (1)", PERMISSION_DENIED);
      }
    }
  }


  @Test
  public void testMultiNodeOwnershipChanges() throws Exception {
    try (Connection connection1 = getConnectionBuilder().withTServer(0).connect();
         Connection connection2 = getConnectionBuilder().withTServer(1).connect();
         Statement statement1 = connection1.createStatement();
         Statement statement2 = connection2.createStatement()) {
      statement1.execute("CREATE ROLE role1");
      statement2.execute("CREATE ROLE role2");

      waitForTServerHeartbeat();

      withRole(statement2, "role2", () -> {
        withRole(statement1, "role1", () -> {
          // Create an object owned by role1.
          statement1.execute("CREATE TABLE test_table(id int)");
        });

        // Fill cache with existing privileges.
        runInvalidQuery(statement2, "SELECT * FROM test_table", PERMISSION_DENIED);

        // Reassign ownership to role2 from connection 1.
        statement1.execute("REASSIGN OWNED BY role1 TO role2");

        waitForTServerHeartbeat();

        // Connection 2 observes the ownership change.
        statement2.execute("ALTER TABLE test_table ADD COLUMN a int");

        // Drop owned objects from connection 1.
        statement1.execute("DROP OWNED BY role2");

        waitForTServerHeartbeat();

        // Connection 2 observes objects dropped.
        runInvalidQuery(
            statement2,
            "ALTER TABLE test_table ADD COLUMN b int",
            "relation \"test_table\" does not exist"
        );

        // Create table from connection 1.
        statement1.execute("CREATE TABLE other_table(id int)");

        // Fill cache with existing privileges.
        runInvalidQuery(statement2, "SELECT * FROM other_table", PERMISSION_DENIED);

        statement1.execute("ALTER TABLE other_table OWNER TO role2");

        waitForTServerHeartbeat();

        // Connection 2 observers owner privileges.
        statement2.execute("DROP TABLE other_table");
      });
    }
  }

  private static void withRoles(
      Statement statement,
      Set<String> roles,
      ThrowingRunnable runnable
  ) throws Exception {
    for (String role : roles) {
      withRole(statement, role, runnable);
    }
  }

  private static void withRole(
      Statement statement,
      String role,
      ThrowingRunnable runnable
  ) throws Exception {
    String sessionUser = getSessionUser(statement);

    statement.execute(String.format("SET SESSION AUTHORIZATION %s", role));
    runnable.run();
    statement.execute(String.format("SET SESSION AUTHORIZATION %s", sessionUser));
  }

  private static class RoleSet extends TreeSet<String> {
    RoleSet(String... roles) {
      super();
      addAll(Lists.newArrayList(roles));
    }

    RoleSet(Set<String> roles) {
      super(roles);
    }

    RoleSet excluding(String... roles) {
      RoleSet newSet = new RoleSet(this);
      newSet.removeAll(Lists.newArrayList(roles));
      return newSet;
    }
  }

  private static String getSessionUser(Statement statement) throws Exception {
    ResultSet resultSet = statement.executeQuery("SELECT SESSION_USER");
    resultSet.next();
    return resultSet.getString(1);
  }

  private static String getCurrentUser(Statement statement) throws Exception {
    ResultSet resultSet = statement.executeQuery("SELECT CURRENT_USER");
    resultSet.next();
    return resultSet.getString(1);
  }
}
