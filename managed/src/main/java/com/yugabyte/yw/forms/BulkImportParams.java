// Copyright (c) YugaByte, Inc.

package com.yugabyte.yw.forms;

import play.data.validation.Constraints;

public class BulkImportParams extends TableManagerParams {
  // Repeating this field here to add constraint
  @Constraints.Required
  public String keyspace;

  @Constraints.Required
  public String tableName;

  // S3 bucket containing data to be bulk imported (should be of format s3://<path>/)
  @Constraints.Required
  public String s3Bucket;

  // Number of task nodes for the EMR job. Optional.
  public int instanceCount = 0;
}
