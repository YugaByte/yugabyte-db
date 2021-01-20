// Copyright (c) Yugabyte, Inc.

package com.yugabyte.yw.forms;

import play.data.validation.Constraints;

import java.util.Date;

/**
 * This class will be used by the API and UI Form Elements to validate constraints for
 * the custom certificate Data.
 */
public class ClientCertParams {
  @Constraints.Required()
  public String username;

  public long certStart = 0L;
  public long certExpiry = 0L;
}
