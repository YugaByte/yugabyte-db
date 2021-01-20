/*
 * Copyright 2020 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 * https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.forms;

import play.data.validation.Constraints;

import java.util.UUID;

/**
 * This class will be used by the API and UI Form Elements to validate constraints are met.
 */
public class AlertDefinitionFormData {
  public enum TemplateType {
    REPLICATION_LAG("max by (node_prefix) (avg_over_time(async_replication_committed_lag_micros" +
      "{node_prefix=\"__nodePrefix__\"}[10m]) or avg_over_time(async_replication_sent_lag_micros" +
      "{node_prefix=\"__nodePrefix__\"}[10m])) / 1000 > __value__");

    private String template;

    public String buildQuery(String nodePrefix, double value) {
      switch (this) {
        case REPLICATION_LAG:
          return template
            .replaceAll("__nodePrefix__", nodePrefix)
            .replaceAll("__value__", Double.toString(value));
        default:
          throw new RuntimeException("Invalid alert definition template provided");
      }
    }

    TemplateType(String template) {
      this.template = template;
    }
  }

  public UUID alertDefinitionUUID;

  public TemplateType template;

  @Constraints.Required()
  public double value;

  @Constraints.Required()
  public String name;

  @Constraints.Required()
  public boolean isActive;
}
