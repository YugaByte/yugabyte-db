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

import com.fasterxml.jackson.databind.JsonNode;
import com.yugabyte.yw.commissioner.AbstractTaskBase;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.common.TableManager;
import com.yugabyte.yw.forms.BackupTableParams;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.models.Backup;
import com.yugabyte.yw.models.Universe;
import play.api.Play;
import play.libs.Json;

import java.util.Map;


public class BackupTable extends AbstractTaskBase {

  Backup backup;

  public BackupTable(Backup backup) {
    this.backup = backup;
  }

  @Override
  protected BackupTableParams taskParams() {
    return (BackupTableParams) taskParams;
  }

  private TableManager tableManager;

  @Override
  public void initialize(ITaskParams params) {
    super.initialize(params);
    tableManager = Play.current().injector().instanceOf(TableManager.class);
  }

  @Override
  public void run() {
    if (backup == null) {
      backup = Backup.fetchByTaskUUID(userTaskUUID);
    }

    try {
      Universe universe = Universe.get(taskParams().universeUUID);
      Map<String, String> config = universe.getConfig();
      if (config.isEmpty() || config.getOrDefault(Universe.TAKE_BACKUPS, "true").equals("true")) {
        if (taskParams().backupList != null) {
          for (BackupTableParams backupParams : taskParams().backupList) {
            ShellResponse response = tableManager.createBackup(backupParams);
            JsonNode jsonNode = Json.parse(response.message);
            if (response.code != 0 || jsonNode.has("error")) {
              LOG.error("Response code={}, hasError={}.", response.code, jsonNode.has("error"));

              throw new RuntimeException(response.message);
            } else {
              LOG.info("[" + getName() + "] STDOUT: " + response.message);
            }
          }

          backup.transitionState(Backup.BackupState.Completed);
        } else {
          ShellResponse response = tableManager.createBackup(taskParams());
          JsonNode jsonNode = Json.parse(response.message);
          if (response.code != 0 || jsonNode.has("error")) {
            LOG.error("Response code={}, hasError={}.", response.code, jsonNode.has("error"));
            backup.transitionState(Backup.BackupState.Failed);
            throw new RuntimeException(response.message);
          } else {
            LOG.info("[" + getName() + "] STDOUT: " + response.message);
            backup.transitionState(Backup.BackupState.Completed);
          }
        }
      } else {
        LOG.info("Skipping table {}:{}", taskParams().keyspace, taskParams().tableName);
        backup.transitionState(Backup.BackupState.Skipped);
      }
    } catch (Exception e) {
      LOG.error("Errored out with: " + e);
      backup.transitionState(Backup.BackupState.Failed);
      throw new RuntimeException(e);
    }
  }
}
