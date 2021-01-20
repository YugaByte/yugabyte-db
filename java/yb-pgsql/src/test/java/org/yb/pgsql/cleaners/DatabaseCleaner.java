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

package org.yb.pgsql.cleaners;

import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

/**
 * Removes all databases excluding `postgres`, `yugabyte`, `system_platform`, `template1`,
 * and `template2`.
 * Any lower-priority cleaners should only clean objects in one of the remaining
 * three databases, or cluster-wide objects (e.g. roles).
 * The passed connection must be open in one of the three databases listed above.
 */
public class DatabaseCleaner implements ClusterCleaner {
  private static final Logger LOG = LoggerFactory.getLogger(DatabaseCleaner.class);

  @Override
  public void clean(Connection connection) throws Exception {
    LOG.info("Cleaning-up non-standard postgres databases");

    try (Statement statement = connection.createStatement()) {
      statement.execute("RESET SESSION AUTHORIZATION");

      ResultSet resultSet = statement.executeQuery(
          "SELECT datname FROM pg_database" +
              " WHERE datname <> 'template0'" +
              " AND datname <> 'template1'" +
              " AND datname <> 'postgres'" +
              " AND datname <> 'yugabyte'" +
              " AND datname <> 'system_platform'");

      List<String> databases = new ArrayList<>();
      while (resultSet.next()) {
        databases.add(resultSet.getString(1));
      }

      for (String database : databases) {
        LOG.info("Dropping database '" + database + "'");
        statement.execute("DROP DATABASE " + database);
      }
    }
  }
}
