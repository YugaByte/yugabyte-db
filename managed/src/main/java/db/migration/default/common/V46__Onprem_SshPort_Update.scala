// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V46__AccessKey_SshPort_Update extends JdbcMigration {
  override def migrate(connection: Connection): Unit = {
    var selectStmt = "SELECT ak.key_code, ak.key_info FROM access_key ak JOIN provider p " +
        "ON p.uuid = ak.provider_uuid AND p.code = 'onprem'"
    var resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val keyCode = resultSet.getString("key_code")
      var keyInfo = Json.parse(resultSet.getString("key_info"))

      val sshPort = new JsNumber(22)
      keyInfo = keyInfo.as[JsObject] + ("sshPort" -> sshPort)

      connection.createStatement().execute("UPDATE access_key SET key_info = " +
        s"'${keyInfo.toString()}' WHERE key_code = '$keyCode'")
    }
  }
}
