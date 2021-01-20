/*
 * Copyright 2019 YugaByte, Inc. and Contributors
 *
 * Licensed under the Polyform Free Trial License 1.0.0 (the "License"); you
 * may not use this file except in compliance with the License. You
 * may obtain a copy of the License at
 *
 *     https://github.com/YugaByte/yugabyte-db/blob/master/licenses/POLYFORM-FREE-TRIAL-LICENSE-1.0.0.txt
 */

package com.yugabyte.yw.commissioner.tasks.subtasks;

import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;
import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

public class InstanceActions extends NodeTaskBase {
  public static final Logger LOG = LoggerFactory.getLogger(InstanceActions.class);

  public NodeManager.NodeCommandType type;

  // Additional parameters for this task.
  public static class Params extends NodeTaskParams {
    // CSV of tag keys to be deleted.
    public String deleteTags = "";
  }

  public InstanceActions(){
    this(NodeManager.NodeCommandType.Tags);
  }

  public InstanceActions(NodeManager.NodeCommandType tasktype){
    type = tasktype;
  }

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public void run() {
    LOG.info("Running Instance action {} type {} against node {}",
            getName(), this.type.toString(), taskParams().nodeName);

    ShellResponse response = getNodeManager().nodeCommand(
        type, taskParams());
    processShellResponse(response);
  }
}
