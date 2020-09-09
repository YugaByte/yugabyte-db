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

import com.yugabyte.yw.common.NodeManager;
import com.yugabyte.yw.common.ShellProcessHandler;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import com.yugabyte.yw.commissioner.tasks.params.NodeTaskParams;

public class AnsibleClusterServerCtl extends NodeTaskBase {

  public static final Logger LOG = LoggerFactory.getLogger(AnsibleClusterServerCtl.class);

  public static class Params extends NodeTaskParams {
    public String process;
    public String command;
    public int sleepAfterCmdMills = 0;
    public boolean isForceDelete = false;
  }

  @Override
  protected Params taskParams() {
    return (Params)taskParams;
  }

  @Override
  public String getName() {
    return super.getName() + "(" + taskParams().nodeName + ", " +
           taskParams().process + ": " + taskParams().command + ")";
  }

  @Override
  public void run() {
    try {
      // Execute the ansible command.
      ShellProcessHandler.ShellResponse response = getNodeManager().nodeCommand(
          NodeManager.NodeCommandType.Control, taskParams());
      logShellResponse(response);
    } catch (Exception e) {
      if (!taskParams().isForceDelete) {
        throw e;
      }
    }

    if (taskParams().sleepAfterCmdMills > 0) {
      try {
        Thread.sleep(taskParams().sleepAfterCmdMills);
      } catch (InterruptedException e) {
        LOG.error("{} Thread Sleep failed: {}", getName(), e.getMessage());
      }
    }
  }
}
