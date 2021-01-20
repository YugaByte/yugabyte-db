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
// Tool to query tablet server operational data

#include <iostream>
#include <memory>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/client/table_handle.h"

#include "yb/common/partition.h"
#include "yb/common/schema.h"
#include "yb/common/wire_protocol.h"
#include "yb/server/server_base.proxy.h"
#include "yb/server/secure.h"
#include "yb/tserver/tserver.pb.h"
#include "yb/tserver/tserver_admin.proxy.h"
#include "yb/tserver/tserver_service.proxy.h"
#include "yb/consensus/consensus.proxy.h"
#include "yb/tserver/tablet_server.h"
#include "yb/util/env.h"
#include "yb/util/faststring.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/protobuf_util.h"
#include "yb/util/net/net_util.h"
#include "yb/rpc/messenger.h"
#include "yb/rpc/rpc_controller.h"
#include "yb/rpc/secure_stream.h"

using std::ostringstream;
using std::shared_ptr;
using std::string;
using std::vector;
using yb::HostPort;
using yb::consensus::ConsensusServiceProxy;
using yb::rpc::Messenger;
using yb::rpc::MessengerBuilder;
using yb::rpc::RpcController;
using yb::server::ServerStatusPB;
using yb::tablet::TabletStatusPB;
using yb::tserver::IsTabletServerReadyRequestPB;
using yb::tserver::IsTabletServerReadyResponsePB;
using yb::tserver::CountIntentsRequestPB;
using yb::tserver::CountIntentsResponsePB;
using yb::tserver::DeleteTabletRequestPB;
using yb::tserver::DeleteTabletResponsePB;
using yb::tserver::FlushTabletsRequestPB;
using yb::tserver::FlushTabletsResponsePB;
using yb::tserver::ListTabletsRequestPB;
using yb::tserver::ListTabletsResponsePB;
using yb::tserver::TabletServerAdminServiceProxy;
using yb::tserver::TabletServerServiceProxy;

const char* const kListTabletsOp = "list_tablets";
const char* const kAreTabletsRunningOp = "are_tablets_running";
const char* const kIsServerReadyOp = "is_server_ready";
const char* const kSetFlagOp = "set_flag";
const char* const kDumpTabletOp = "dump_tablet";
const char* const kTabletStateOp = "get_tablet_state";
const char* const kDeleteTabletOp = "delete_tablet";
const char* const kCurrentHybridTime = "current_hybrid_time";
const char* const kStatus = "status";
const char* const kCountIntents = "count_intents";
const char* const kFlushTabletOp = "flush_tablet";
const char* const kFlushAllTabletsOp = "flush_all_tablets";
const char* const kCompactTabletOp = "compact_tablet";
const char* const kCompactAllTabletsOp = "compact_all_tablets";

DEFINE_string(server_address, "localhost",
              "Address of server to run against");
DEFINE_int64(timeout_ms, 1000 * 60, "RPC timeout in milliseconds");

DEFINE_bool(force, false, "If true, allows the set_flag command to set a flag "
            "which is not explicitly marked as runtime-settable. Such flag changes may be "
            "simply ignored on the server, or may cause the server to crash.");

DEFINE_string(certs_dir_name, "",
              "Directory with certificates to use for secure server connection.");

PB_ENUM_FORMATTERS(yb::consensus::LeaderLeaseStatus);

// Check that the value of argc matches what's expected, otherwise return a
// non-zero exit code. Should be used in main().
#define CHECK_ARGC_OR_RETURN_WITH_USAGE(op, expected) \
  do { \
    const string& _op = (op); \
    const int _expected = (expected); \
    if (argc != _expected) { \
      /* We substract 2 from _expected because we don't want to count argv[0] or [1]. */ \
      std::cerr << "Invalid number of arguments for " << _op \
                << ": expected " << (_expected - 2) << " arguments" << std::endl; \
      google::ShowUsageWithFlagsRestrict(argv[0], __FILE__); \
      return 2; \
    } \
  } while (0);

// Invoke 'to_call' and check its result. If it failed, print 'to_prepend' and
// the error to cerr and return a non-zero exit code. Should be used in main().
#define RETURN_NOT_OK_PREPEND_FROM_MAIN(to_call, to_prepend) \
  do { \
    ::yb::Status s = (to_call); \
    if (!s.ok()) { \
      std::cerr << (to_prepend) << ": " << s.ToString() << std::endl; \
      return 1; \
    } \
  } while (0);

namespace yb {
namespace tools {

typedef ListTabletsResponsePB::StatusAndSchemaPB StatusAndSchemaPB;

class TsAdminClient {
 public:
  // Creates an admin client for host/port combination e.g.,
  // "localhost" or "127.0.0.1:7050".
  TsAdminClient(std::string addr, int64_t timeout_millis);

  ~TsAdminClient();

  // Initialized the client and connects to the specified tablet
  // server.
  Status Init();

  // Sets 'tablets' a list of status information for all tablets on a
  // given tablet server.
  Status ListTablets(std::vector<StatusAndSchemaPB>* tablets);

  // Gets the number of tablets waiting to be bootstrapped and prints to console.
  Status GetNumUnbootstrappedTablets(int64_t* num_unbootstrapped_tablets);

  // Sets the gflag 'flag' to 'val' on the remote server via RPC.
  // If 'force' is true, allows setting flags even if they're not marked as
  // safe to change at runtime.
  Status SetFlag(const string& flag, const string& val,
                 bool force);

  // Get the schema for the given tablet.
  Status GetTabletSchema(const std::string& tablet_id, SchemaPB* schema);

  // Dump the contents of the given tablet, in key order, to the console.
  Status DumpTablet(const std::string& tablet_id);

  // Print the consensus state to the console.
  Status PrintConsensusState(const std::string& tablet_id);

  // Delete a tablet replica from the specified peer.
  // The 'reason' string is passed to the tablet server, used for logging.
  Status DeleteTablet(const std::string& tablet_id,
                      const std::string& reason);

  // Sets hybrid_time to the value of the tablet server's current hybrid_time.
  Status CurrentHybridTime(uint64_t* hybrid_time);

  // Get the server status
  Status GetStatus(ServerStatusPB* pb);

  // Count write intents on all tablets.
  Status CountIntents(int64_t* num_intents);

  // Flush or compact a given tablet on a given tablet server.
  // If 'tablet_id' is empty string, flush or compact all tablets.
  Status FlushTablets(const std::string& tablet_id, bool is_compaction);

 private:
  std::string addr_;
  MonoDelta timeout_;
  bool initted_;
  std::unique_ptr<rpc::SecureContext> secure_context_;
  std::unique_ptr<rpc::Messenger> messenger_;
  shared_ptr<server::GenericServiceProxy> generic_proxy_;
  gscoped_ptr<tserver::TabletServerServiceProxy> ts_proxy_;
  gscoped_ptr<tserver::TabletServerAdminServiceProxy> ts_admin_proxy_;
  gscoped_ptr<consensus::ConsensusServiceProxy> cons_proxy_;

  DISALLOW_COPY_AND_ASSIGN(TsAdminClient);
};

TsAdminClient::TsAdminClient(string addr, int64_t timeout_millis)
    : addr_(std::move(addr)),
      timeout_(MonoDelta::FromMilliseconds(timeout_millis)),
      initted_(false) {}

TsAdminClient::~TsAdminClient() {
  if (messenger_) {
    messenger_->Shutdown();
  }
}

Status TsAdminClient::Init() {
  CHECK(!initted_);

  HostPort host_port;
  RETURN_NOT_OK(host_port.ParseString(addr_, tserver::TabletServer::kDefaultPort));
  auto messenger_builder = MessengerBuilder("ts-cli");
  if (!FLAGS_certs_dir_name.empty()) {
    secure_context_ = VERIFY_RESULT(server::CreateSecureContext(FLAGS_certs_dir_name));
    server::ApplySecureContext(secure_context_.get(), &messenger_builder);
  }
  messenger_ = VERIFY_RESULT(messenger_builder.Build());

  rpc::ProxyCache proxy_cache(messenger_.get());

  generic_proxy_.reset(new server::GenericServiceProxy(&proxy_cache, host_port));
  ts_proxy_.reset(new TabletServerServiceProxy(&proxy_cache, host_port));
  ts_admin_proxy_.reset(new TabletServerAdminServiceProxy(&proxy_cache, host_port));
  cons_proxy_.reset(new ConsensusServiceProxy(&proxy_cache, host_port));
  initted_ = true;

  VLOG(1) << "Connected to " << addr_;

  return Status::OK();
}

Status TsAdminClient::ListTablets(vector<StatusAndSchemaPB>* tablets) {
  CHECK(initted_);

  ListTabletsRequestPB req;
  ListTabletsResponsePB resp;
  RpcController rpc;

  rpc.set_timeout(timeout_);
  RETURN_NOT_OK(ts_proxy_->ListTablets(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  tablets->assign(resp.status_and_schema().begin(), resp.status_and_schema().end());

  return Status::OK();
}

Status TsAdminClient::GetNumUnbootstrappedTablets(int64_t* num_unbootstrapped_tablets) {
  CHECK(initted_);

  IsTabletServerReadyRequestPB req;
  IsTabletServerReadyResponsePB resp;
  RpcController rpc;

  rpc.set_timeout(timeout_);
  RETURN_NOT_OK(ts_proxy_->IsTabletServerReady(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  std::cout << resp.num_tablets_not_running() << "/" << resp.total_tablets()
            << " tablets are not yet bootstrapped" << std::endl;
  *num_unbootstrapped_tablets = resp.num_tablets_not_running();

  return Status::OK();
}

Status TsAdminClient::SetFlag(const string& flag, const string& val,
                              bool force) {
  server::SetFlagRequestPB req;
  server::SetFlagResponsePB resp;
  RpcController rpc;

  rpc.set_timeout(timeout_);
  req.set_flag(flag);
  req.set_value(val);
  req.set_force(force);

  RETURN_NOT_OK(generic_proxy_->SetFlag(req, &resp, &rpc));
  switch (resp.result()) {
    case server::SetFlagResponsePB::SUCCESS:
      return Status::OK();
    case server::SetFlagResponsePB::NOT_SAFE:
      return STATUS(RemoteError, resp.msg() + " (use --force flag to allow anyway)");
    default:
      return STATUS(RemoteError, resp.ShortDebugString());
  }
}

Status TsAdminClient::GetTabletSchema(const std::string& tablet_id,
                                      SchemaPB* schema) {
  VLOG(1) << "Fetching schema for tablet " << tablet_id;
  vector<StatusAndSchemaPB> tablets;
  RETURN_NOT_OK(ListTablets(&tablets));
  for (const StatusAndSchemaPB& pair : tablets) {
    if (pair.tablet_status().tablet_id() == tablet_id) {
      *schema = pair.schema();
      return Status::OK();
    }
  }
  return STATUS(NotFound, "Cannot find tablet", tablet_id);
}

Status TsAdminClient::PrintConsensusState(const std::string& tablet_id) {
  ServerStatusPB status_pb;
  RETURN_NOT_OK(GetStatus(&status_pb));

  consensus::GetConsensusStateRequestPB cons_reqpb;
  cons_reqpb.set_dest_uuid(status_pb.node_instance().permanent_uuid());
  cons_reqpb.set_tablet_id(tablet_id);

  consensus::GetConsensusStateResponsePB cons_resp_pb;
  RpcController rpc;
  RETURN_NOT_OK_PREPEND(
      cons_proxy_->GetConsensusState(cons_reqpb, &cons_resp_pb, &rpc),
      "Failed to query tserver for consensus state");
  std::cout << "Lease-Status"
            << "\t\t"
            << " Leader-UUID ";
  std::cout << PBEnumToString(cons_resp_pb.leader_lease_status()) << "\t\t"
            << cons_resp_pb.cstate().leader_uuid();

  return Status::OK();
}

Status TsAdminClient::DumpTablet(const std::string& tablet_id) {
  SchemaPB schema_pb;
  RETURN_NOT_OK(GetTabletSchema(tablet_id, &schema_pb));
  Schema schema;
  RETURN_NOT_OK(SchemaFromPB(schema_pb, &schema));

  tserver::ReadRequestPB req;
  tserver::ReadResponsePB resp;

  req.set_tablet_id(tablet_id);
  RpcController rpc;
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK_PREPEND(ts_proxy_->Read(req, &resp, &rpc), "Read() failed");

  if (resp.has_error()) {
    return STATUS(IOError, "Failed to read: ", resp.error().ShortDebugString());
  }

  QLRowBlock row_block(schema);
  Slice data = VERIFY_RESULT(rpc.GetSidecar(0));
  if (!data.empty()) {
    RETURN_NOT_OK(row_block.Deserialize(YQL_CLIENT_CQL, &data));
  }

  for (const auto& row : row_block.rows()) {
    std::cout << row.ToString() << std::endl;
  }

  return Status::OK();
}

Status TsAdminClient::DeleteTablet(const string& tablet_id,
                                   const string& reason) {
  ServerStatusPB status_pb;
  RETURN_NOT_OK(GetStatus(&status_pb));

  DeleteTabletRequestPB req;
  DeleteTabletResponsePB resp;
  RpcController rpc;

  req.set_tablet_id(tablet_id);
  req.set_dest_uuid(status_pb.node_instance().permanent_uuid());
  req.set_reason(reason);
  req.set_delete_type(tablet::TABLET_DATA_TOMBSTONED);
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK_PREPEND(ts_admin_proxy_->DeleteTablet(req, &resp, &rpc),
                        "DeleteTablet() failed");

  if (resp.has_error()) {
    return STATUS(IOError, "Failed to delete tablet: ",
                           resp.error().ShortDebugString());
  }
  return Status::OK();
}

Status TsAdminClient::CurrentHybridTime(uint64_t* hybrid_time) {
  server::ServerClockRequestPB req;
  server::ServerClockResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK(generic_proxy_->ServerClock(req, &resp, &rpc));
  CHECK(resp.has_hybrid_time()) << resp.DebugString();
  *hybrid_time = resp.hybrid_time();
  return Status::OK();
}

Status TsAdminClient::GetStatus(ServerStatusPB* pb) {
  server::GetStatusRequestPB req;
  server::GetStatusResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK(generic_proxy_->GetStatus(req, &resp, &rpc));
  CHECK(resp.has_status()) << resp.DebugString();
  pb->Swap(resp.mutable_status());
  return Status::OK();
}

Status TsAdminClient::CountIntents(int64_t* num_intents) {
  CountIntentsRequestPB req;
  CountIntentsResponsePB resp;
  RpcController rpc;
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK(ts_admin_proxy_->CountIntents(req, &resp, &rpc));
  *num_intents = resp.num_intents();
  return Status::OK();
}

Status TsAdminClient::FlushTablets(const std::string& tablet_id, bool is_compaction) {
  ServerStatusPB status_pb;
  RETURN_NOT_OK(GetStatus(&status_pb));

  FlushTabletsRequestPB req;
  FlushTabletsResponsePB resp;
  RpcController rpc;

  if (!tablet_id.empty()) {
    req.add_tablet_ids(tablet_id);
    req.set_all_tablets(false);
  } else {
    req.set_all_tablets(true);
  }
  req.set_dest_uuid(status_pb.node_instance().permanent_uuid());
  req.set_is_compaction(is_compaction);
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK_PREPEND(ts_admin_proxy_->FlushTablets(req, &resp, &rpc),
                        "FlushTablets() failed");

  if (resp.has_error()) {
    return STATUS(IOError, "Failed to flush tablet: ",
                           resp.error().ShortDebugString());
  }
  return Status::OK();
}

namespace {

void SetUsage(const char* argv0) {
  ostringstream str;

  str << argv0 << " [--server_address=<addr>] <operation> <flags>\n"
      << "<operation> must be one of:\n"
      << "  " << kListTabletsOp << "\n"
      << "  " << kAreTabletsRunningOp << "\n"
      << "  " << kIsServerReadyOp << "\n"
      << "  " << kSetFlagOp << " [-force] <flag> <value>\n"
      << "  " << kTabletStateOp << " <tablet_id>\n"
      << "  " << kDumpTabletOp << " <tablet_id>\n"
      << "  " << kDeleteTabletOp << " <tablet_id> <reason string>\n"
      << "  " << kCurrentHybridTime << "\n"
      << "  " << kStatus << "\n"
      << "  " << kCountIntents << "\n"
      << "  " << kFlushTabletOp << " <tablet_id>\n"
      << "  " << kFlushAllTabletsOp << "\n"
      << "  " << kCompactTabletOp << " <tablet_id>\n"
      << "  " << kCompactAllTabletsOp << "\n";
  google::SetUsageMessage(str.str());
}

string GetOp(int argc, char** argv) {
  if (argc < 2) {
    google::ShowUsageWithFlagsRestrict(argv[0], __FILE__);
    exit(1);
  }

  return argv[1];
}

} // anonymous namespace

static int TsCliMain(int argc, char** argv) {
  FLAGS_logtostderr = 1;
  FLAGS_minloglevel = 2;
  SetUsage(argv[0]);
  ParseCommandLineFlags(&argc, &argv, true);
  InitGoogleLoggingSafe(argv[0]);
  const string addr = FLAGS_server_address;

  string op = GetOp(argc, argv);

  TsAdminClient client(addr, FLAGS_timeout_ms);

  RETURN_NOT_OK_PREPEND_FROM_MAIN(client.Init(),
                                  "Unable to establish connection to " + addr);

  // TODO add other operations here...
  if (op == kListTabletsOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    vector<StatusAndSchemaPB> tablets;
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.ListTablets(&tablets),
                                    "Unable to list tablets on " + addr);
    for (const StatusAndSchemaPB& status_and_schema : tablets) {
      Schema schema;
      RETURN_NOT_OK_PREPEND_FROM_MAIN(SchemaFromPB(status_and_schema.schema(), &schema),
                                      "Unable to deserialize schema from " + addr);
      PartitionSchema partition_schema;
      RETURN_NOT_OK_PREPEND_FROM_MAIN(PartitionSchema::FromPB(status_and_schema.partition_schema(),
                                                              schema, &partition_schema),
                                      "Unable to deserialize partition schema from " + addr);


      TabletStatusPB ts = status_and_schema.tablet_status();

      Partition partition;
      Partition::FromPB(ts.partition(), &partition);

      string state = tablet::RaftGroupStatePB_Name(ts.state());
      std::cout << "Tablet id: " << ts.tablet_id() << std::endl;
      std::cout << "State: " << state << std::endl;
      std::cout << "Table name: " << ts.table_name() << std::endl;
      std::cout << "Partition: " << partition_schema.PartitionDebugString(partition, schema)
                << std::endl;
      std::cout << "Schema: " << schema.ToString() << std::endl;
    }
  } else if (op == kAreTabletsRunningOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    vector<StatusAndSchemaPB> tablets;
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.ListTablets(&tablets),
                                    "Unable to list tablets on " + addr);
    bool all_running = true;
    for (const StatusAndSchemaPB& status_and_schema : tablets) {
      TabletStatusPB ts = status_and_schema.tablet_status();
      if (ts.state() != tablet::RUNNING) {
        std::cout << "Tablet id: " << ts.tablet_id() << " is "
                  << tablet::RaftGroupStatePB_Name(ts.state()) << std::endl;
        all_running = false;
      }
    }

    if (all_running) {
      std::cout << "All tablets are running" << std::endl;
    } else {
      std::cout << "Not all tablets are running" << std::endl;
      return 1;
    }
  } else if (op == kIsServerReadyOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    int64_t unbootstrapped_tablets;
    RETURN_NOT_OK_PREPEND_FROM_MAIN(
        client.GetNumUnbootstrappedTablets(&unbootstrapped_tablets), "Unable to read server state");

    if (unbootstrapped_tablets > 0) {
      std::cout << "Tablet server is not ready" << std::endl;
      return 1;
    } else {
      std::cout << "Tablet server is ready" << std::endl;
    }
  } else if (op == kSetFlagOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 4);

    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.SetFlag(argv[2], argv[3], FLAGS_force),
                                    "Unable to set flag");

  } else if (op == kTabletStateOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 3);

    string tablet_id = argv[2];
    RETURN_NOT_OK_PREPEND_FROM_MAIN(
        client.PrintConsensusState(tablet_id), "Unable to print tablet state");
  } else if (op == kDumpTabletOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 3);

    string tablet_id = argv[2];
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.DumpTablet(tablet_id),
                                    "Unable to dump tablet");
  } else if (op == kDeleteTabletOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 4);

    string tablet_id = argv[2];
    string reason = argv[3];

    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.DeleteTablet(tablet_id, reason),
                                    "Unable to delete tablet");
  } else if (op == kCurrentHybridTime) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    uint64_t hybrid_time;
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.CurrentHybridTime(&hybrid_time),
                                    "Unable to get hybrid_time");
    std::cout << hybrid_time << std::endl;
  } else if (op == kStatus) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    ServerStatusPB status;
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.GetStatus(&status),
                                    "Unable to get status");
    std::cout << status.DebugString() << std::endl;
  } else if (op == kCountIntents) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);
    int64_t num_intents = 0;

    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.CountIntents(&num_intents),
                                    "Unable to count intents");

    std::cout << num_intents << std::endl;
  } else if (op == kFlushTabletOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 3);

    string tablet_id = argv[2];
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.FlushTablets(tablet_id, false /* is_compaction */),
                                    "Unable to flush tablet");
  } else if (op == kFlushAllTabletsOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.FlushTablets(std::string(), false /* is_compaction */),
                                    "Unable to flush all tablets");
  } else if (op == kCompactTabletOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 3);

    string tablet_id = argv[2];
    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.FlushTablets(tablet_id, true /* is_compaction */),
                                    "Unable to compact tablet");
  } else if (op == kCompactAllTabletsOp) {
    CHECK_ARGC_OR_RETURN_WITH_USAGE(op, 2);

    RETURN_NOT_OK_PREPEND_FROM_MAIN(client.FlushTablets(std::string(), true /* is_compaction */),
                                    "Unable to compact all tablets");
  } else {
    std::cerr << "Invalid operation: " << op << std::endl;
    google::ShowUsageWithFlagsRestrict(argv[0], __FILE__);
    return 2;
  }

  return 0;
}

} // namespace tools
} // namespace yb

int main(int argc, char** argv) {
  return yb::tools::TsCliMain(argc, argv);
}
