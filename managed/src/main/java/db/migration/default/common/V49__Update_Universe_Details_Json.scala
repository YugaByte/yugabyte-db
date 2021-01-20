// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V49__Update_Universe_Details_Json extends JdbcMigration {
  override def migrate(connection: Connection): Unit = {
    val selectStmt = "SELECT universe_uuid, universe_details_json " +
      "FROM universe"

    val resultSet = connection.createStatement().executeQuery(selectStmt)

    val communicationPortsSeq = Seq(
      "masterHttpPort" -> JsNumber(7000),
      "masterRpcPort" -> JsNumber(7100),
      "tserverHttpPort" -> JsNumber(9000),
      "tserverRpcPort" -> JsNumber(9100),
      "redisServerHttpPort" -> JsNumber(11000),
      "redisServerRpcPort" -> JsNumber(6379),
      "yqlServerHttpPort" -> JsNumber(12000),
      "yqlServerRpcPort" -> JsNumber(9042),
      "ysqlServerHttpPort" -> JsNumber(13000),
      "ysqlServerRpcPort" -> JsNumber(5433),
      "nodeExporterPort" -> JsNumber(9300)
    )

    val communicationPortsJson = ("communicationPorts" -> JsObject(communicationPortsSeq))

    while (resultSet.next()) {
      val univUuid = resultSet.getString("universe_uuid")
      val univDetails = Json.parse(resultSet.getString("universe_details_json"))

      val newUnivDetails = univDetails.as[JsObject] + communicationPortsJson

      connection.createStatement().execute(s"UPDATE universe SET universe_details_json = " +
        s"'$newUnivDetails' WHERE universe_uuid = '$univUuid'")
    }
  }
}
