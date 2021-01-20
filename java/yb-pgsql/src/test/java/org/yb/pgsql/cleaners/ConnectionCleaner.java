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
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

/**
 * Closes all registered postgres connections, except the connection passed
 * to {@link ConnectionCleaner#clean(Connection)}.
 */
public class ConnectionCleaner implements ClusterCleaner {
  private static final Logger LOG = LoggerFactory.getLogger(ConnectionCleaner.class);

  private static List<Connection> connectionsToClose = new ArrayList<>();

  public static void register(Connection connection) {
    connectionsToClose.add(connection);
  }

  @Override
  public void clean(Connection rootConnection) throws Exception {
    LOG.info("Cleaning-up postgres connections");

    if (rootConnection != null) {
      try (Statement statement = rootConnection.createStatement()) {
        try (ResultSet resultSet = statement.executeQuery(
            "SELECT client_hostname, client_port, state, query, pid FROM pg_stat_activity")) {
          while (resultSet.next()) {
            int backendPid = resultSet.getInt(5);
            LOG.info(String.format(
                "Found connection: hostname=%s, port=%s, state=%s, query=%s, backend_pid=%s",
                resultSet.getString(1), resultSet.getInt(2),
                resultSet.getString(3), resultSet.getString(4), backendPid));
          }
        }
      } catch (SQLException e) {
        LOG.info("Exception when trying to list PostgreSQL connections", e);
      }

      LOG.info("Closing connections.");
      for (Connection connection : connectionsToClose) {
        // Keep the main connection alive between tests.
        if (connection == rootConnection) continue;

        try {
          if (connection == null) {
            LOG.error("connectionsToClose contains a null connection!");
          } else {
            // TODO(dmitry): Workaround for #1721, remove after fix.
            try (Statement statement = connection.createStatement()) {
              statement.execute("ROLLBACK");
              statement.execute("DISCARD TEMP");
            } catch (SQLException ex) {
              // Exception is acceptable only in case connection was already closed.
              // Connection state is not checked prior to execute statement
              // due to possible race condition in case connection is closed by server side.
              if (!connection.isClosed()) {
                throw ex;
              }
            }
            connection.close();

          }
        } catch (SQLException ex) {
          LOG.error("Exception while trying to close connection");
          throw ex;
        }
      }
    } else {
      LOG.info("Connection is already null, nothing to close");
    }
    LOG.info("Finished closing connection.");

    connectionsToClose.clear();
  }
}
