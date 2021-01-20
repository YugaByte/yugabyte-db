// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import java.util.UUID
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V52__Update_Access_Key_Create_Extra_Migrations extends JdbcMigration {

  override def migrate(connection: Connection): Unit = {
    val selectStmt = "SELECT * FROM access_key"
    val resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val keyCode = resultSet.getString("key_code")
      val keyInfo = Json.parse(resultSet.getString("key_info"))
      var skipProvision : Boolean = false;

      val preProvisionScript = keyInfo \ "provisionInstanceScript"
      if (preProvisionScript.isInstanceOf[JsDefined]
          && preProvisionScript.as[String].trim.nonEmpty) {
        skipProvision = true;
      }

      val newKeyInfo = keyInfo.as[JsObject] + ("skipProvisioning" -> Json.toJson(skipProvision));
      connection.createStatement().execute(s"UPDATE access_key SET key_info = " +
        s"'$newKeyInfo' WHERE key_code = '$keyCode'")

    }
    connection.createStatement().execute(s"CREATE table extra_migration " +
      s"(migration varchar(256) not null)")
    connection.createStatement().execute(s"INSERT INTO extra_migration VALUES " +
      s"('V52__Update_Access_Key_Create_Extra_Migration')")
  }
}
