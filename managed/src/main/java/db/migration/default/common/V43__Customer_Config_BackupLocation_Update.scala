// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V43__Customer_Config_BackupLocation_Update extends JdbcMigration {
  override def migrate(connection: Connection): Unit = {
    val selectStmt = "SELECT config_uuid, data FROM customer_config WHERE type = 'STORAGE'"
    val resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val configUuid = resultSet.getString("config_uuid")
      var data = Json.parse(resultSet.getString("data"))
      var backup_location : JsValue = JsNull
      val keyList = List("NFS_PATH", "S3_BUCKET")
      for (key <- keyList) {
        if ((data \ key).isInstanceOf[JsDefined]) {
            backup_location = (data \ key).get
            data = data.as[JsObject] - key
        }
      }
      data = data.as[JsObject] + ("BACKUP_LOCATION" -> backup_location)
      connection.createStatement().execute(s"UPDATE customer_config SET data = " +
        s"'${data.toString()}' WHERE config_uuid = '$configUuid'")
    }
  }
}
