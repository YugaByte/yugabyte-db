// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.models.helpers;

import static org.junit.Assert.assertNotEquals;

import org.junit.Test;

import com.yugabyte.yw.models.Alert;
import com.yugabyte.yw.models.CustomerTask;

public class DataConvertersTest {

  @Test
  public void testTaskTargetToAlertTargetType() {
    // We are checking that all known task types are mapped. Otherwise we'll get
    // Alert.TargetType.TaskType as a result.
    for (CustomerTask.TargetType type : CustomerTask.TargetType.values()) {
      assertNotEquals(Alert.TargetType.TaskType, DataConverters.taskTargetToAlertTargetType(type));
    }
  }
}
