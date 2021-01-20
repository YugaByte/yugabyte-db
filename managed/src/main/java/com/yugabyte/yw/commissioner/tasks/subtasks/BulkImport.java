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

import com.yugabyte.yw.commissioner.AbstractTaskBase;
import com.yugabyte.yw.common.ShellProcessHandler;
import com.yugabyte.yw.common.ShellResponse;
import com.yugabyte.yw.common.TableManager;
import com.yugabyte.yw.forms.BulkImportParams;
import com.yugabyte.yw.forms.ITaskParams;
import com.yugabyte.yw.forms.TableDefinitionTaskParams;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import play.api.Play;

public class BulkImport extends AbstractTaskBase {

  public static final Logger LOG = LoggerFactory.getLogger(BulkImport.class);

  private TableManager tableManager;

  @Override
  protected BulkImportParams taskParams() {
    return (BulkImportParams) taskParams;
  }

  @Override
  public void initialize(ITaskParams params) {
    super.initialize(params);
    tableManager = Play.current().injector().instanceOf(TableManager.class);
  }

  @Override
  public void run() {
    // Execute the ansible command and log its result.
    processShellResponse(tableManager.bulkImport(taskParams()));
  }
}
