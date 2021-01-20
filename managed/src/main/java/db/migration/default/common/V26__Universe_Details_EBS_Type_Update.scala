// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import java.sql.Connection
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V26__Universe_Details_EBS_Type_UUID_Update extends JdbcMigration {

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

  def renameEBSType(json: JsValue, path: Array[Any]): JsValue = {
    path match {
      // Match every path that has a proper ebsType path
      case Array("clusters", _ : Integer, "userIntent") if (json \ "deviceInfo" \ "ebsType").isInstanceOf[JsDefined] =>
        val deviceInfo = json \ "deviceInfo"
        val storageType = deviceInfo \ "ebsType"
        val provider = json \ "providerType"
        var newStorageValue : JsValue = storageType.get
        if (newStorageValue == JsNull && provider.isInstanceOf[JsDefined]) {
          provider.get.as[String] match {
            case "gcp" =>
              newStorageValue = Json.toJson("Scratch")
            case "aws" =>
              newStorageValue = Json.toJson("GP2")
            case _ =>
          }
        }
        json.as[JsObject] - "deviceInfo" + ("deviceInfo" -> (deviceInfo.as[JsObject] - "ebsType" + ("storageType" -> newStorageValue)))
      case _ =>
        json
    }
  }

  override def migrate(connection: Connection): Unit = {
    val selectStmt = "SELECT universe_uuid, universe_details_json FROM universe"
    val resultSet = connection.createStatement().executeQuery(selectStmt)

    while (resultSet.next()) {
      val univUuid = resultSet.getString("universe_uuid")
      val univDetails = Json.parse(resultSet.getString("universe_details_json"))
      val newUnivDetails = processJson(univDetails)(renameEBSType).toString()
      connection.createStatement().execute(s"UPDATE universe SET universe_details_json = " +
        s"'$newUnivDetails' WHERE universe_uuid = '$univUuid'")
    }
  }
}

