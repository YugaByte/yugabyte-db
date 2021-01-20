// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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
#ifndef YB_TSERVER_TSERVER_PATH_HANDLERS_H
#define YB_TSERVER_TSERVER_PATH_HANDLERS_H

#include <string>
#include <sstream>
#include <vector>

#include "yb/gutil/macros.h"
#include "yb/server/webserver.h"

namespace yb {

class Schema;

namespace consensus {
class ConsensusStatePB;
} // namespace consensus

namespace tserver {

class TabletServer;

class TabletServerPathHandlers {
 public:
  explicit TabletServerPathHandlers(TabletServer* tserver)
    : tserver_(tserver) {
  }

  ~TabletServerPathHandlers();

  CHECKED_STATUS Register(Webserver* server);

 private:
  void HandleTablesPage(const Webserver::WebRequest& req,
                        Webserver::WebResponse* resp);
  void HandleTabletsPage(const Webserver::WebRequest& req,
                         Webserver::WebResponse* resp);
  void HandleOperationsPage(const Webserver::WebRequest& req,
                            Webserver::WebResponse* resp);
  void HandleDashboardsPage(const Webserver::WebRequest& req,
                            Webserver::WebResponse* resp);
  void HandleMaintenanceManagerPage(const Webserver::WebRequest& req,
                                    Webserver::WebResponse* resp);
  void HandleHealthCheck(const Webserver::WebRequest& req,
                         Webserver::WebResponse* resp);
  void HandleVersionInfoDump(const Webserver::WebRequest& req,
                              Webserver::WebResponse* resp);
  std::string ConsensusStatePBToHtml(const consensus::ConsensusStatePB& cstate) const;
  std::string GetDashboardLine(const std::string& link,
                               const std::string& text, const std::string& desc);

  TabletServer* const tserver_;

  DISALLOW_COPY_AND_ASSIGN(TabletServerPathHandlers);
};

} // namespace tserver
} // namespace yb
#endif /* YB_TSERVER_TSERVER_PATH_HANDLERS_H */
