// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
package org.yb.util;

public class TableProperties {
  public static final int TP_NO_FLAGS          = 0;
  public static final int TP_YCQL              = TP_NO_FLAGS;
  public static final int TP_YSQL              = 1; // bit 0
  public static final int TP_NON_TRANSACTIONAL = TP_NO_FLAGS;
  public static final int TP_TRANSACTIONAL     = 2; // bit 1
  public static final int TP_DIRECT_QUERY      = TP_NO_FLAGS;
  public static final int TP_PREPARED_QUERY    = 4; // bit 2
  public static final int TP_NON_COLOCATED     = TP_NO_FLAGS;
  public static final int TP_COLOCATED         = 8; // bit 3

  public int flags;
  public TableProperties() { flags = TP_NO_FLAGS; }
  public TableProperties(int f) { flags = f; }
  public Boolean isYSQLTable() throws Exception {
    return (flags & TP_YSQL) == TP_YSQL;
  }
  public Boolean isTransactional() throws Exception {
    return (flags & TP_TRANSACTIONAL) == TP_TRANSACTIONAL;
  }
  public Boolean usePreparedQueries() throws Exception {
    return (flags & TP_PREPARED_QUERY) == TP_PREPARED_QUERY;
  }
  public Boolean isColocated() throws Exception {
    return (flags & TP_COLOCATED) == TP_COLOCATED;
  }
};
