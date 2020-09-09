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

// C wrappers around "pggate" for using it from external tools.

#ifndef YB_YQL_PGGATE_YBC_PGGATE_TOOL_H
#define YB_YQL_PGGATE_YBC_PGGATE_TOOL_H

#include "yb/common/ybc_util.h"

#ifdef __cplusplus
extern "C" {
#endif

// Setup the master IP(s) before calling YBCInitPgGate().
void YBCSetMasterAddresses(const char* hosts);

YBCStatus YBCInitPgGateBackend();

void YBCShutdownPgGateBackend();

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // YB_YQL_PGGATE_YBC_PGGATE_TOOL_H
