// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V16__PlacementUtil_UUID_Update extends JdbcMigration {

  /**
    * Utility method to recursively apply a modification function to a json value and all its
    * elements. Modification function is applied top-down (i.e. applied to parents before children).
    *
    * @param json     the json value
    * @param path     the location of the current value inside the root json object
    * @param modifier the function to be applied to every json value, takes the current path as the
    *                 second argument to decide if/how to modify the current value.
    * @return the modified json value
    */
  def processJson(json: JsValue, path: Array[Any] = Array())
                 (implicit modifier: (JsValue, Array[Any]) => JsValue): JsValue = {
    modifier(json, path) match {
      case JsObject(underlying) =>
        JsObject(underlying.map(kv => kv._1 -> processJson(kv._2, path :+ kv._1)))
      case JsArray(value) =>
        JsArray(value.zipWithIndex.map(vi => processJson(vi._1, path :+ vi._2)))
      case jsval => jsval
    }
  }

  def addPlacementUuid(primaryClusterUuid: String)(json: JsValue, path: Array[Any]): JsValue = {
    path match {
      case Array("nodeDetailsSet", _) if !json.as[JsObject].keys.contains("placementUuid") =>
        json.as[JsObject] + ("placementUuid" -> JsString(primaryClusterUuid))
      case _ => json
    }
  }

  override def migrate(connection: Connection): Unit = {
    val selectStmt = "SELECT universe_uuid, universe_details_json FROM universe"
    val resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val univUuid = resultSet.getString("universe_uuid")
      val univDetails = Json.parse(resultSet.getString("universe_details_json"))
      val clusters = (univDetails \ "clusters").get.as[JsArray].value
      val primaryClusterId = clusters.find(c => (c \ "clusterType").get.equals(JsString("PRIMARY")))
                                     .map(c => (c \ "uuid").get.as[JsString].value).get
      val newUnivDetails = processJson(univDetails)(addPlacementUuid(primaryClusterId)).toString()
      connection.createStatement().execute(s"UPDATE universe SET universe_details_json = " +
        s"'$newUnivDetails' WHERE universe_uuid = '$univUuid'")
    }
  }
}
