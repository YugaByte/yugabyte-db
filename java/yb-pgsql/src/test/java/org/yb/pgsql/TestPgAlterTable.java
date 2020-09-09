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

import java.sql.Connection;
import java.sql.Statement;

import static org.yb.pgsql.IsolationLevel.SERIALIZABLE;

@RunWith(value = YBTestRunnerNonTsanOnly.class)
public class TestPgAlterTable extends BasePgSQLTest {
  private static final Logger LOG = LoggerFactory.getLogger(TestPgAlterTable.class);

  @Test
  public void testAddColumnIfNotExists() throws Exception {
    testAddColumnIfNotExistsInternal(true);
  }

  @Test
  public void testAddIfNotExists() throws Exception {
    testAddColumnIfNotExistsInternal(false);
  }

  private void testAddColumnIfNotExistsInternal(boolean column) throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");

      String alterPrefix = String.format(
          "ALTER TABLE test_table ADD%s IF NOT EXISTS",
          column ? " COLUMN" : ""
      );

      // None of the following should produce an error.
      statement.execute(alterPrefix + " id varchar");
      statement.execute(alterPrefix + " name varchar");
      statement.execute(alterPrefix + " name int");
      statement.execute(alterPrefix + " oid int");

      // Table has expected columns and types.
      assertQuery(
          statement,
          selectAttributesQuery("test_table"),
          new Row("id", "int4"),
          new Row("name", "varchar"),
          new Row("oid", "int4")
      );

      // Selecting from table returns correct columns.
      statement.execute("INSERT INTO test_table(id, name, oid) VALUES (1, 'some name', 2)");
      assertQuery(statement, "SELECT * FROM test_table", new Row(1, "some name", 2));
    }
  }

  @Test
  public void testDropColumnIfExists() throws Exception {
    testDropColumnIfExistsInternal(true);
  }

  @Test
  public void testDropIfExists() throws Exception {
    testDropColumnIfExistsInternal(false);
  }

  private void testDropColumnIfExistsInternal(boolean column) throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(a int, b int, c int)");

      String alterPrefix = String.format(
          "ALTER TABLE test_table DROP%s IF EXISTS",
          column ? " COLUMN" : ""
      );

      // None of the following should produce an error.
      statement.execute(alterPrefix + " a");
      statement.execute(alterPrefix + " a");
      statement.execute(alterPrefix + " f");
      statement.execute(alterPrefix + " c");

      // Table has expected columns and types.
      assertQuery(
          statement,
          selectAttributesQuery("test_table"),
          new Row("b", "int4")
      );

      // Selecting from table returns correct columns.
      statement.execute("INSERT INTO test_table(b) VALUES (1)");
      assertQuery(statement, "SELECT * FROM test_table", new Row(1));
    }
  }

  @Test
  public void testAddColumnWithNotNullConstraint() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");

      statement.execute("ALTER TABLE test_table ADD a int NOT NULL");
      statement.execute("ALTER TABLE test_table ADD b int NULL");

      statement.execute("INSERT INTO test_table(id, a, b) VALUES (1, 2, 3)");
      statement.execute("INSERT INTO test_table(a, b) VALUES (2, 3)");
      statement.execute("INSERT INTO test_table(a) VALUES (2)");

      runInvalidQuery(
          statement,
          "INSERT INTO test_table(id, b) VALUES(1, 3)",
          "null value in column \"a\" violates not-null constraint"
      );

      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD c int NOT NULL",
          "column \"c\" contains null values"
      );
    }
  }

  @Test
  public void testAddColumnWithCheckConstraint() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");

      statement.execute("ALTER TABLE test_table ADD a int CHECK (a > 0)");

      statement.execute("INSERT INTO test_table(a) VALUES (1)");
      statement.execute("INSERT INTO test_table(a) VALUES (2)");
      statement.execute("INSERT INTO test_table(a) VALUES (3)");

      runInvalidQuery(
          statement,
          "INSERT INTO test_table(a) VALUES(0)",
          "new row for relation \"test_table\" violates check constraint"
      );

      runInvalidQuery(
          statement,
          "INSERT INTO test_table(a) VALUES(-10)",
          "new row for relation \"test_table\" violates check constraint"
      );

      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD b int CHECK (b IS NOT NULL)",
          "check constraint \"test_table_b_check\" is violated by some row"
      );
    }
  }

  @Test
  public void testAddColumnWithForeignKeyConstraint() throws Exception {
    try (Connection connection = getConnectionBuilder().withIsolationLevel(SERIALIZABLE).connect();
         Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int UNIQUE, pk int PRIMARY KEY)");

      statement.execute(
          "CREATE TABLE ref_table(id int, id9 int REFERENCES test_table(id) ON UPDATE CASCADE)");

      statement.execute("ALTER TABLE ref_table ADD id1 int REFERENCES test_table(id)");

      // Foreign key behaves correctly.
      runInvalidQuery(
          statement,
          "INSERT INTO ref_table(id, id1) VALUES (1, 1)",
          "violates foreign key constraint \"ref_table_id1_fkey\""
      );
      statement.execute("INSERT INTO test_table(id, pk) VALUES (2, 4)");
      statement.execute("INSERT INTO ref_table(id, id1) VALUES (1, 2)");

      // Can add referencing column to non-empty table.
      statement.execute("ALTER TABLE ref_table ADD id2 int REFERENCES test_table(id) MATCH SIMPLE");
      statement.execute("ALTER TABLE ref_table ADD id3 int REFERENCES test_table(id) MATCH FULL");

      // Can add table reference.
      statement.execute("ALTER TABLE ref_table ADD id4 int REFERENCES test_table");
      runInvalidQuery(
          statement,
          "INSERT INTO ref_table(id4) VALUES (2)",
          "violates foreign key constraint \"ref_table_id4_fkey\""
      );
      statement.execute("INSERT INTO ref_table(id4) VALUES (4)");
    }
  }

  @Test
  public void testAddColumnWithForeignKeyConstraintOnDelete() throws Exception {
    try (Connection connection = getConnectionBuilder().withIsolationLevel(SERIALIZABLE).connect();
         Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(pk int PRIMARY KEY, id int UNIQUE)");

      statement.execute("CREATE TABLE ref_table(id int)");

      /*
       * CASCADE
       */

      statement.execute("ALTER TABLE ref_table ADD id_cascade int" +
          " REFERENCES test_table(id) ON DELETE CASCADE");

      statement.execute("INSERT INTO test_table VALUES (1, 1)");
      statement.execute("INSERT INTO ref_table(id, id_cascade) VALUES (1, 1)");

      statement.execute("DELETE FROM test_table WHERE id = 1");

      // Rows in both tables were dropped.
      assertQuery(statement, "SELECT id_cascade FROM ref_table WHERE id = 1");
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 1");

      /*
       * RESTRICT
       */

      statement.execute("ALTER TABLE ref_table ADD id_restrict int" +
              " REFERENCES test_table(id) ON DELETE RESTRICT");

      statement.execute("INSERT INTO test_table VALUES (2, 2)");
      statement.execute("INSERT INTO ref_table(id, id_restrict) VALUES (2, 2)");

      runInvalidQuery(
          statement,
          "DELETE FROM test_table WHERE pk = 2",
          "violates foreign key constraint \"ref_table_id_restrict_fkey\" on table \"ref_table\""
      );

      // Neither tables dropped rows.
      assertQuery(statement, "SELECT id_restrict FROM ref_table WHERE id = 2", new Row(2));
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 2", new Row(2));

      /*
       * NO ACTION
       */

      statement.execute("ALTER TABLE ref_table ADD id_no_action int" +
          " REFERENCES test_table(id) ON DELETE NO ACTION");

      statement.execute("INSERT INTO test_table VALUES (3, 3)");
      statement.execute("INSERT INTO ref_table(id, id_no_action) VALUES (3, 3)");

      runInvalidQuery(
          statement,
          "DELETE FROM test_table WHERE pk = 3",
          "violates foreign key constraint \"ref_table_id_no_action_fkey\" on table \"ref_table\""
      );

      // Neither tables dropped rows.
      assertQuery(statement, "SELECT id_no_action FROM ref_table WHERE id = 3", new Row(3));
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 3", new Row(3));

      /*
       * SET NULL
       */

      statement.execute("ALTER TABLE ref_table ADD id_set_null int" +
          " REFERENCES test_table(id) ON DELETE SET NULL");

      statement.execute("INSERT INTO test_table VALUES (4, 4)");
      statement.execute("INSERT INTO ref_table(id, id_set_null) VALUES (4, 4)");

      statement.execute("DELETE FROM test_table WHERE id = 4");

      // The referencing row is set to null, and the referenced row is deleted.
      assertQuery(
          statement,
          "SELECT id_set_null FROM ref_table WHERE id = 4",
          new Row((Integer) null)
      );
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 4");
    }
  }

  @Test
  public void testAddColumnWithForeignKeyConstraintOnUpdate() throws Exception {
    try (Connection connection = getConnectionBuilder().withIsolationLevel(SERIALIZABLE).connect();
         Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(pk int PRIMARY KEY, id int UNIQUE)");

      statement.execute("CREATE TABLE ref_table(id int)");

      /*
       * CASCADE
       */

      statement.execute("ALTER TABLE ref_table ADD id_cascade int" +
          " REFERENCES test_table(id) ON UPDATE CASCADE");

      statement.execute("INSERT INTO test_table VALUES (1, 1)");
      statement.execute("INSERT INTO ref_table(id, id_cascade) VALUES (1, 1)");

      statement.execute("UPDATE test_table SET id = 11 WHERE pk = 1");

      // Both rows were updated.
      assertQuery(statement, "SELECT id_cascade FROM ref_table WHERE id = 1", new Row(11));
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 1", new Row(11));

      /*
       * RESTRICT
       */

      statement.execute("ALTER TABLE ref_table ADD id_restrict int" +
          " REFERENCES test_table(id) ON UPDATE RESTRICT");

      statement.execute("INSERT INTO test_table VALUES (2, 2)");
      statement.execute("INSERT INTO ref_table(id, id_restrict) VALUES (2, 2)");

      runInvalidQuery(
          statement,
          "UPDATE test_table SET id = 12 WHERE pk = 2",
          "violates foreign key constraint \"ref_table_id_restrict_fkey\" on table \"ref_table\""
      );

      // Neither row was modified.
      assertQuery(statement, "SELECT id_restrict FROM ref_table WHERE id = 2", new Row(2));
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 2", new Row(2));

      /*
       * NO ACTION
       */

      statement.execute("ALTER TABLE ref_table ADD id_no_action int" +
          " REFERENCES test_table(id) ON UPDATE NO ACTION");

      statement.execute("INSERT INTO test_table VALUES (3, 3)");
      statement.execute("INSERT INTO ref_table(id, id_no_action) VALUES (3, 3)");

      runInvalidQuery(
          statement,
          "UPDATE test_table SET id = 13 WHERE pk = 3",
          "violates foreign key constraint \"ref_table_id_no_action_fkey\" on table \"ref_table\""
      );

      // Neither row was modified.
      assertQuery(statement, "SELECT id_no_action FROM ref_table WHERE id = 3", new Row(3));
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 3", new Row(3));

      /*
       * SET NULL
       */

      statement.execute("ALTER TABLE ref_table ADD id_set_null int" +
          " REFERENCES test_table(id) ON UPDATE SET NULL");

      statement.execute("INSERT INTO test_table VALUES (4, 4)");
      statement.execute("INSERT INTO ref_table(id, id_set_null) VALUES (4, 4)");

      statement.execute("UPDATE test_table SET id = 14 WHERE pk = 4");

      // Referenced row was updated and referencing row was set to null.
      assertQuery(
          statement,
          "SELECT id_set_null FROM ref_table WHERE id = 4",
          new Row((Integer) null)
      );
      assertQuery(statement, "SELECT id FROM test_table WHERE pk = 4", new Row(14));
    }
  }

  @Test
  public void testAddColumnWithUnsupportedConstraint() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");

      // All variants of UNIQUE fail.
      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD COLUMN a int UNIQUE",
          "This ALTER TABLE command is not yet supported"
      );
      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD a int UNIQUE",
          "This ALTER TABLE command is not yet supported"
      );
      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD COLUMN IF NOT EXISTS a int UNIQUE",
          "This ALTER TABLE command is not yet supported"
      );
      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD IF NOT EXISTS a int UNIQUE",
          "This ALTER TABLE command is not yet supported"
      );

      // PRIMARY KEY fails.
      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD a int PRIMARY KEY",
          "This ALTER TABLE command is not yet supported"
      );

      // GENERATED fails.
      runInvalidQuery(
          statement,
          "ALTER TABLE test_table ADD gac int GENERATED ALWAYS AS IDENTITY",
          "This ALTER TABLE command is not yet supported"
      );

      // No columns were added.
      assertQuery(
          statement,
          selectAttributesQuery("test_table"),
          new Row("id", "int4")
      );
    }
  }

  @Test
  public void testRenameTableIfExists() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");

      // No-op with no error if table does not exist.
      statement.execute("ALTER TABLE IF EXISTS other_table RENAME TO new_table");
      runInvalidQuery(statement, "SELECT * FROM new_table", "does not exist");

      // Table is renamed if it exists.
      statement.execute("ALTER TABLE IF EXISTS test_table RENAME TO new_table");
      statement.execute("SELECT * FROM new_table");
      runInvalidQuery(statement, "SELECT * FROM test_table", "does not exist");
    }
  }

  @Test
  public void testSetWithoutOids() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");

      // Alter is a no-op on user tables (which do not have oids).
      statement.execute("ALTER TABLE test_table SET WITHOUT OIDS");

      // Alter not allowed on system catalogs (which have oids).
      runInvalidQuery(
          statement,
          "ALTER TABLE pg_class SET WITHOUT OIDS",
          "permission denied: \"pg_class\" is a system catalog"
      );
    }
  }

  @Test
  public void testAddColumnWithDefault() throws Exception {
    try (Statement statement = connection.createStatement()) {
      statement.execute("CREATE TABLE test_table(id int)");
      statement.execute("ALTER TABLE test_table ADD a int DEFAULT 11");
      statement.execute("ALTER TABLE test_table ADD b int DEFAULT null");

      // Check that defaults are generated as defined.
      statement.execute("INSERT INTO test_table(id) values (1)");
      assertQuery(statement, "SELECT a,b FROM test_table WHERE id = 1", new Row(11, null));
      statement.execute("INSERT INTO test_table(id, b) values (2, 0)");
      assertQuery(statement, "SELECT a,b FROM test_table WHERE id = 2", new Row(11, 0));
      statement.execute("INSERT INTO test_table(id, a) values (3, 22)");
      assertQuery(statement, "SELECT a,b FROM test_table WHERE id = 3", new Row(22, null));
    }
  }

  private static String selectAttributesQuery(String table) {
    return String.format(
        "SELECT a.attname, t.typname FROM pg_attribute a" +
            " JOIN pg_class r ON r.oid=a.attrelid" +
            " JOIN pg_type t ON t.oid=a.atttypid" +
            " WHERE a.attnum > 0 AND r.relname='%s'",
        table
    );
  }
}
