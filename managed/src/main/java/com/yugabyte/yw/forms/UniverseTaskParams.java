// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import java.util.UUID;

public class UniverseTaskParams implements ITaskParams {
  // The universe against which this node's details should be saved.
  public UUID universeUUID;

  // Expected version of the universe for operation execution. Set to -1 if an operation should
  // not verify expected version of the universe.
  public int expectedUniverseVersion;
}
