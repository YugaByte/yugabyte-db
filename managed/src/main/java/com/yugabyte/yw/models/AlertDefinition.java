/*
 * Copyright 2020 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.models;

import io.ebean.Finder;
import io.ebean.Model;
import play.data.validation.Constraints;

import javax.persistence.*;
import java.util.Set;
import java.util.UUID;

@Entity
public class AlertDefinition extends Model {
  @Constraints.Required
  @Id
  @Column(nullable = false, unique = true)
  public UUID uuid;

  @Constraints.Required
  @Column(columnDefinition = "Text", nullable = false)
  public String name;

  @Constraints.Required
  @Column(columnDefinition = "Text", nullable = false)
  public UUID universeUUID;

  @Constraints.Required
  @Column(columnDefinition = "Text", nullable = false)
  public String query;

  @Constraints.Required
  public boolean isActive;

  @Constraints.Required
  @Column(nullable = false)
  public UUID customerUUID;

  private static final Finder<UUID, AlertDefinition> find =
    new Finder<UUID, AlertDefinition>(AlertDefinition.class) {};

  public static AlertDefinition create(
    UUID customerUUID,
    UUID universeUUID,
    String name,
    String query,
    boolean isActive
  ) {
    AlertDefinition definition = new AlertDefinition();
    definition.uuid = UUID.randomUUID();
    definition.name = name;
    definition.customerUUID = customerUUID;
    definition.universeUUID = universeUUID;
    definition.query = query;
    definition.isActive = isActive;
    definition.save();

    return definition;
  }

  public static AlertDefinition get(UUID alertDefinitionUUID) {
    return find.query().where().idEq(alertDefinitionUUID).findOne();
  }

  public static AlertDefinition get(UUID customerUUID, UUID universeUUID, String name) {
    return find.query().where()
      .eq("customer_uuid", customerUUID)
      .eq("universe_uuid", universeUUID)
      .eq("name", name)
      .findOne();
  }

  public static AlertDefinition update(
    UUID alertDefinitionUUID,
    String query,
    boolean isActive
  ) {
    AlertDefinition alertDefinition = get(alertDefinitionUUID);
    alertDefinition.query = query;
    alertDefinition.isActive = isActive;
    alertDefinition.save();

    return alertDefinition;
  }

  public static Set<AlertDefinition> listActive(UUID customerUUID) {
    return find.query().where()
      .eq("customer_uuid", customerUUID)
      .eq("is_active", true)
      .findSet();
  }
}
