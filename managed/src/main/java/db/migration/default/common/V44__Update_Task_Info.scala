// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V44__Update_Task_Info extends JdbcMigration {
  override def migrate(connection: Connection): Unit = {
    val selectStmt = "SELECT uuid, details FROM task_info WHERE task_type = 'UpgradeUniverse'"
    val resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val uuid = resultSet.getString("uuid")
      var details = Json.parse(resultSet.getString("details"))
      if ((details \ "rollingUpgrade").isInstanceOf[JsDefined]) {
        val rollingUpgrade = (details \ "rollingUpgrade").as[Boolean]
        details = details.as[JsObject] - "rollingUpgrade"
        val upgradeOption : String = if (rollingUpgrade) "Rolling" else "Non-Rolling"
        details = details.as[JsObject] + ("upgradeOption" -> Json.toJson(upgradeOption))
        connection.createStatement().execute(s"UPDATE task_info SET details = " +
                                             s"'${details.toString()}' WHERE uuid = '$uuid'")
      }
    }
  }
}
