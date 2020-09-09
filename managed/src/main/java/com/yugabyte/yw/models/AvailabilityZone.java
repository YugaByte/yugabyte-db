// Copyright (c) Yugabyte, Inc.
package com.yugabyte.yw.models;

import java.util.HashMap;
import java.util.Map;
import java.util.List;
import java.util.UUID;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;
import javax.persistence.ManyToOne;

import io.ebean.annotation.DbJson;
import io.ebean.*;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.annotation.JsonBackReference;
import com.fasterxml.jackson.annotation.JsonIgnore;

import play.data.validation.Constraints;
import play.libs.Json;

import static com.yugabyte.yw.models.helpers.CommonUtils.maskConfig;

@Entity
public class AvailabilityZone extends Model {

  @Id
  public UUID uuid;

  @Column(length = 25, nullable = false)
  public String code;

  @Column(length = 100, nullable = false)
  @Constraints.Required
  public String name;

  @Constraints.Required
  @Column(nullable = false)
  @ManyToOne
  @JsonBackReference
  public Region region;

  @Column(nullable = false, columnDefinition = "boolean default true")
  public Boolean active = true;
  public Boolean isActive() { return active; }
  public void setActiveFlag(Boolean active) { this.active = active; }

  @Column(length = 50)
  public String subnet;

  @DbJson
  @Column(columnDefinition = "TEXT")
  public JsonNode config;

  public void setConfig(Map<String, String> configMap) {
    Map<String, String> currConfig = this.getConfig();
    for (String key : configMap.keySet()) {
      currConfig.put(key, configMap.get(key));
    }
    this.config = Json.toJson(currConfig);
    this.save();
  }

  @JsonIgnore
  public JsonNode getMaskedConfig() {
    if (this.config == null) {
      return Json.newObject();
    } else {
      return maskConfig(this.config);
    }
  }

  @JsonIgnore
  public Map<String, String> getConfig() {
    if (this.config == null) {
      return new HashMap();
    } else {
      return Json.fromJson(this.config, Map.class);
    }
  }

  /**
   * Query Helper for Availability Zone with primary key
   */
  public static final Finder<UUID, AvailabilityZone> find =
    new Finder<UUID,AvailabilityZone>(AvailabilityZone.class){};

  public static AvailabilityZone create(Region region, String code, String name, String subnet) {
    AvailabilityZone az = new AvailabilityZone();
    az.region = region;
    az.code = code;
    az.name = name;
    az.subnet = subnet;
    az.save();
    return az;
  }

  public static List<AvailabilityZone> getAZsForRegion(UUID regionUUID) {
    return find.query().where().eq("region_uuid", regionUUID).findList();
  }

  public static AvailabilityZone getByCode(String code) {
    return find.query().where().eq("code", code).findOne();
  }

  public static AvailabilityZone get(UUID zoneUuid) {
    return AvailabilityZone.find.query().
      fetch("region")
      .fetch("region.provider")
      .where()
      .idEq(zoneUuid)
      .findOne();
  }

  @JsonBackReference
  public Provider getProvider() {
    String providerQuery =
        "select p.uuid, p.code, p.name from provider p where p.uuid = :p_uuid";
    RawSql rawSql = RawSqlBuilder.parse(providerQuery).create();
    Query<Provider> query = Ebean.find(Provider.class);
    query.setRawSql(rawSql);
    query.setParameter("p_uuid", region.provider.uuid);
    return query.findOne();
  }
}
