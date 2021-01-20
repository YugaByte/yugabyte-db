// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V45__AccessKey_SshPort_Update extends JdbcMigration {
  override def migrate(connection: Connection): Unit = {
    // Create ssh port in the access_key table.
    var selectStmt = "SELECT key_code, key_info FROM access_key"
    var resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val keyCode = resultSet.getString("key_code")
      var keyInfo = Json.parse(resultSet.getString("key_info"))

      val sshPort = new JsNumber(54422)
      keyInfo = keyInfo.as[JsObject] + ("sshPort" -> sshPort)

      connection.createStatement().execute("UPDATE access_key SET key_info = " +
        s"'${keyInfo.toString()}' WHERE key_code = '$keyCode'")
    }

    // Delete unused ssh port from node_instances table.
    selectStmt = "SELECT node_uuid, node_details_json FROM node_instance"
    resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val nodeUuid = resultSet.getString("node_uuid")
      var nodeDetails = Json.parse(resultSet.getString("node_details_json"))
      nodeDetails = nodeDetails.as[JsObject] - "sshPort"
      connection.createStatement().execute("UPDATE node_instance SET node_details_json = " +
        s"'${nodeDetails.toString()}' WHERE node_uuid = '$nodeUuid'")
    }
  }
}
