// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models;

import javax.persistence.Column;
import javax.persistence.Entity;
import javax.persistence.Id;

import com.fasterxml.jackson.databind.JsonNode;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import io.ebean.*;
import io.ebean.annotation.*;

import play.data.validation.Constraints;

/**
 * Table that has all the configuration data for Yugaware, such as the latest stable release version
 * of YB to deploy, etc. Each entry is a name-value pair.
 */
@Entity
public class YugawareProperty extends Model {
  public static final Logger LOG = LoggerFactory.getLogger(YugawareProperty.class);

  // The name of the property.
  @Id
  private String name;

  // The types of entries in this table.
  private enum PropertyEntryType {
    // Externally specified configuration properties. E.g. supported machine types, etc.
    @EnumValue("Config")
    Config,

    // System maintained properties. E.g. system maintained stats on instance provision time.
    @EnumValue("System")
    System,
  }
  @Constraints.Required
  @Column(nullable = false)
  private PropertyEntryType type;

  // The property config.
  @Constraints.Required
  @Column(columnDefinition = "TEXT")
  private JsonNode value;
  public JsonNode getValue() { return value; }

  // The property description.
  @Column(columnDefinition = "TEXT")
  private String description;

  public static final Finder<String, YugawareProperty> find =
    new Finder<String, YugawareProperty>(YugawareProperty.class){};

  private YugawareProperty(String name, PropertyEntryType type, JsonNode value, String description) {
    this.name = name;
    this.type = type;
    this.value = value;
    this.description = description;
  }

  public static YugawareProperty get(String name) {
    return find.byId(name);
  }

  /**
   * These are externally specified configuration properties. These are not updated by the system at
   * runtime. They can be modified while the system is running, and will take effect from the next
   * task that runs.
   *
   * @param name is the property name
   * @param value is the property value as JsonNode
   * @param description is a description of the property
   */
  public static void addConfigProperty(String name, JsonNode value, String description) {
    YugawareProperty entry = find.byId(name);
    if (entry == null) {
      entry = new YugawareProperty(name, PropertyEntryType.Config, value, description);
      entry.save();
    } else {
      entry.value = value;
      entry.update();
    }
  }
}
