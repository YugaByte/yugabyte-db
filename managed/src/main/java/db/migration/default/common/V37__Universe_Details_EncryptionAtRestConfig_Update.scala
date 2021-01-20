// Copyright (c) YugaByte, Inc.

package db.migration.default.common

import com.fasterxml.jackson.databind.ObjectMapper
import com.fasterxml.jackson.databind.node.ObjectNode
import com.yugabyte.yw.common.kms.util.AwsEARServiceUtil
import com.yugabyte.yw.common.kms.util.EncryptionAtRestUtil
import com.yugabyte.yw.common.kms.util.KeyProvider
import com.yugabyte.yw.models.Universe
import java.sql.Connection
import java.util.UUID
import play.api.libs.json._

import org.flywaydb.core.api.migration.jdbc.JdbcMigration

class V37__Universe_Details_EncryptionAtRestConfig_Update extends JdbcMigration {
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

  def updateUserIntent(userIntent: JsValue): JsValue = {
    var result = userIntent.as[JsObject]
    if (result.keys.contains("enableEncryptionAtRest")) result -= "enableEncryptionAtRest"
    result
  }

    def updateEncryptionAtRestConfig(
                                      connection: Connection,
                                      config: JsValue,
                                      universeUuid: String,
                                      customerUuid: Option[String],
                                      universeName: Option[String]
                                    ): JsValue = {
      var result = JsObject(Seq())
      customerUuid match {
        case Some(customerUuidValue) => {
          // This is the case for a universe that has been/currently is encrypted at rest
          // We should map the key provider to a new KMS Configuration
          val keyProvider = (config \ "kms_provider").get.as[JsString].value
          val newConfigName = s"${universeName.getOrElse("")} - kms config"
          // Try and retrieve any existing KMS Configurations
          val existingConfigSet = connection.createStatement().executeQuery(
            s"SELECT config_uuid, auth_config FROM kms_config WHERE key_provider = '$keyProvider'" +
              s" AND customer_uuid = '$customerUuidValue' AND version = 1 LIMIT 1"
          )
          while (existingConfigSet.next()) {
            val mapper = new ObjectMapper()
            val oldConfigUuid = UUID.fromString(existingConfigSet.getString("config_uuid"))
            val authConfig = mapper.readTree(
              existingConfigSet.getString("auth_config")
            ).asInstanceOf[ObjectNode]
            val unmaskedAuthConfig = EncryptionAtRestUtil.unmaskConfigData(
              UUID.fromString(customerUuidValue),
              authConfig,
              Enum.valueOf(classOf[KeyProvider], keyProvider)
            )
            // If the provider is AWS, we need to move the CMK ID to the KMS Configuration
            if (keyProvider.equals("AWS")) {
              val universeAlias = AwsEARServiceUtil.getAlias(
                oldConfigUuid,
                AwsEARServiceUtil.generateAliasName(universeUuid)
              )
              val cmkId = universeAlias.getTargetKeyId()
              unmaskedAuthConfig.asInstanceOf[ObjectNode].put("cmk_id", cmkId)
            }
            val maskedAuthConfig = EncryptionAtRestUtil.maskConfigData(
              UUID.fromString(customerUuidValue),
              unmaskedAuthConfig,
              Enum.valueOf(classOf[KeyProvider], keyProvider)
            )
            val newConfigUuid = UUID.randomUUID()
            // Create new KMS Configuration for each encrypted universe
            connection.createStatement().execute(s"INSERT INTO kms_config (config_uuid, " +
              s"customer_uuid, key_provider, auth_config, version, name) VALUES " +
              s"('${newConfigUuid.toString()}', '$customerUuidValue', '$keyProvider', " +
              s"'${maskedAuthConfig.toString()}', 2, '$newConfigName')")
            result = config.as[JsObject]
            if (!result.keys.contains("kmsConfigUUID")) {
              result += ("kmsConfigUUID" -> JsString(newConfigUuid.toString()))
            }
            if (!result.keys.contains("encryptionAtRestEnabled")) {
              var encryptionAtRestEnabled = false
              val clusterArrayStmt = "SELECT universe_details_json::json -> 'clusters' AS clusters FROM universe";
              val clusterArraySet = connection.createStatement().executeQuery(clusterArrayStmt)
              while (clusterArraySet.next()) {
                Json.parse(clusterArraySet.getString("clusters")).as[Seq[JsObject]]
                  .filter{ obj => (obj \ "clusterType").isInstanceOf[JsDefined] && (obj \ "clusterType").as[String].equals("PRIMARY") }
                  .filter{ obj => (obj \ "userIntent").isInstanceOf[JsDefined] && (obj \ "userIntent" \ "enableEncryptionAtRest").isInstanceOf[JsDefined] }
                  .map{ _ \ "userIntent" \ "enableEncryptionAtRest" }
                  .foreach{ result => encryptionAtRestEnabled = result.as[Boolean] }
              }
              result += ("encryptionAtRestEnabled" -> JsBoolean(encryptionAtRestEnabled))
            }
            // Remove encryptionAtRest fields no longer needed
            if (result.keys.contains("kms_provider")) result -= "kms_provider"
            if (result.keys.contains("cmk_policy")) result -= "cmk_policy"
            if (result.keys.contains("algorithm")) result -= "algorithm"
            if (result.keys.contains("key_size")) result -= "key_size"
          }
        }
        case _ => {
          // This is the case for a universe that has never been encrypted at rest
          result = JsObject(Seq("encryptionAtRestEnabled" -> JsBoolean(false)))
        }
      }
      result
    }

    def updateUniverseDetails(
                               connection: Connection,
                               universeUuid: String,
                               customerUuid: Option[String],
                               encryptionAtRestConfig: Boolean,
                               universeName: Option[String]
                             )(json: JsValue, path: Array[Any]): JsValue = {
      path match {
        case Array("encryptionAtRestConfig") if encryptionAtRestConfig => updateEncryptionAtRestConfig(
          connection,
          json,
          universeUuid,
          customerUuid,
          universeName
        )
        case Array("clusters", _ : Integer, "userIntent") if !encryptionAtRestConfig => updateUserIntent(json)
        case _ => json
      }
    }

    override def migrate(connection: Connection): Unit = {
      val encryptedSelectStmt = "SELECT name, universe_uuid, customer_id, universe_details_json " +
        "FROM universe WHERE universe_uuid IN (SELECT DISTINCT target_uuid FROM kms_history " +
        "WHERE type = 'UNIVERSE_KEY')"
      val encryptedResultSet = connection.createStatement().executeQuery(encryptedSelectStmt)
      while (encryptedResultSet.next()) {
        val univUuid = encryptedResultSet.getString("universe_uuid")
        val univName = encryptedResultSet.getString("name")
        val customerId = encryptedResultSet.getString("customer_id")
        val univDetails = Json.parse(encryptedResultSet.getString("universe_details_json"))
        val customerSet = connection.createStatement().executeQuery(s"SELECT uuid from customer " +
          s"WHERE id = '$customerId' LIMIT 1")
        while (customerSet.next()) {
          val customerUuid = customerSet.getString("uuid")
          val newUnivDetails = processJson(univDetails)(
            updateUniverseDetails(connection, univUuid, Some(customerUuid), true, Some(univName))
          ).toString()
          connection.createStatement().execute(s"UPDATE universe SET universe_details_json = " +
            s"'$newUnivDetails' WHERE universe_uuid = '$univUuid'")
        }
      }

      val unencryptedSelectStatement = "SELECT universe_uuid, universe_details_json FROM " +
        "universe WHERE universe_uuid NOT IN (SELECT DISTINCT target_uuid FROM kms_history WHERE type = " +
        "'UNIVERSE_KEY')"
      val unencryptedResultSet = connection
        .createStatement()
        .executeQuery(unencryptedSelectStatement)
      while (unencryptedResultSet.next()) {
        val univUuid = unencryptedResultSet.getString("universe_uuid")
        val univDetails = Json.parse(unencryptedResultSet.getString("universe_details_json"))
        val newUnivDetails = processJson(univDetails)(
          updateUniverseDetails(connection, univUuid, None, true, None)
        ).toString()
        connection.createStatement().execute(s"UPDATE universe SET universe_details_json = " +
          s"'$newUnivDetails' WHERE universe_uuid = '$univUuid'")
      }

      val allUniverseResultSet = connection.createStatement()
        .executeQuery("SELECT universe_uuid, universe_details_json FROM universe")
      while (allUniverseResultSet.next()) {
        val univUuid = allUniverseResultSet.getString("universe_uuid")
        val univDetails = Json.parse(allUniverseResultSet.getString("universe_details_json"))
        val newUnivDetails = processJson(univDetails)(
          updateUniverseDetails(connection, univUuid, None, false, None)
        ).toString()
        connection.createStatement().execute(s"UPDATE universe SET universe_details_json = " +
          s"'$newUnivDetails' WHERE universe_uuid = '$univUuid'")
      }
    }
}
