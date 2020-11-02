// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import com.fasterxml.jackson.annotation.JsonFormat;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import io.ebean.*;
import io.ebean.annotation.EnumValue;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.data.validation.Constraints;
import play.libs.Json;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Enumerated;
import javax.persistence.EnumType;
import javax.persistence.Id;

import java.util.*;

@Entity
public class Alert extends Model {

  /**
   * These are the possible targets for the alert.
   */
  public enum TargetType {
    @EnumValue("UniverseType")
    UniverseType;

    public Class getType() {
      switch (this) {
        case UniverseType:
          return Universe.class;
        default:
          return null;
      }
    }

    public static TargetType getType(CustomerTask.TargetType targetType) {
      switch (targetType) {
        case Universe:
          return UniverseType;
        default:
          return null;
      }
    }
  }

  @Constraints.Required
  @Id
  @Column(nullable = false, unique = true)
  public UUID uuid;

  @Constraints.Required
  @Column(nullable = false)
  public UUID customerUUID;

  // UUID of the target type if the alert is associated with one.
  public UUID targetUUID;

  // The target type.
  @Enumerated(EnumType.STRING)
  public TargetType targetType;

  @Constraints.Required
  @Column(nullable = false)
  @JsonFormat(shape = JsonFormat.Shape.STRING, pattern = "yyyy-MM-dd HH:mm:ss")
  private Date createTime;

  @Constraints.Required
  @Column(columnDefinition = "Text", nullable = false)
  public String errCode;

  @Constraints.Required
  @Column(length = 255)
  public String type;

  @Constraints.Required
  @Column(columnDefinition = "Text", nullable = false)
  public String message;

  public static final Logger LOG = LoggerFactory.getLogger(Alert.class);
  private static final Finder<UUID, Alert> find = new Finder<UUID, Alert>(Alert.class) {};

  public static Alert create(
    UUID customerUUID, UUID targetUUID, TargetType targetType,String errCode,
    String type, String message) {
    Alert alert = new Alert();
    alert.uuid = UUID.randomUUID();
    alert.customerUUID = customerUUID;
    alert.targetUUID = targetUUID;
    alert.targetType = targetType;
    alert.createTime = new Date();
    alert.errCode = errCode;
    alert.type = type;
    alert.message = message;
    alert.save();
    return alert;
  }

  public static Alert create(UUID customerUUID, String errCode, String type, String message) {
    return Alert.create(customerUUID, null, null, errCode, type, message);
  }

  public void update(String newMessage) {
    createTime = new Date();
    message = newMessage;
    save();
  }

  public JsonNode toJson() {
    ObjectNode json = Json.newObject()
      .put("uuid", uuid.toString())
      .put("customerUUID", customerUUID.toString())
      .put("createTime", createTime.toString())
      .put("errCode", errCode)
      .put("type", type)
      .put("message", message);
    return json;
  }

  public static Boolean exists(String errCode) {
    return find.query().where().eq("errCode", errCode).findCount() != 0;
  }

  public static Boolean exists(String errCode, UUID targetUUID) {
    return find.query().where().eq("errCode", errCode)
                               .eq("target_uuid", targetUUID).findCount() != 0;
  }

  public static List<Alert> list(UUID customerUUID) {
    return find.query().where()
      .eq("customer_uuid", customerUUID)
      .orderBy("create_time desc")
      .findList();
  }

  public static List<Alert> list(UUID customerUUID, String errCode) {
    return find.query().where().eq("customer_uuid", customerUUID)
                               .eq("errCode", errCode).findList();
  }

  public static Alert get(UUID customerUUID, String errCode, UUID targetUUID) {
    return find.query().where().eq("customer_uuid", customerUUID)
                               .eq("errCode", errCode)
                               .eq("target_uuid", targetUUID).findOne();
  }

  public static Alert get(UUID alertUUID) {
    return find.query().where().idEq(alertUUID).findOne();
  }
}
