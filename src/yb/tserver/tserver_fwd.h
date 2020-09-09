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

#ifndef YB_TSERVER_TSERVER_FWD_H
#define YB_TSERVER_TSERVER_FWD_H

namespace yb {
namespace tserver {

class GetTabletStatusRequestPB;
class GetTabletStatusResponsePB;
class LocalTabletServer;
class TabletServer;
class TabletPeerLookupIf;
class TabletServerOptions;
class TabletServerServiceProxy;
class TSTabletManager;

} // namespace tserver
} // namespace yb

#endif // YB_TSERVER_TSERVER_FWD_H
