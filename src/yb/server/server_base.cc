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
#include "yb/server/server_base.h"

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <gflags/gflags.h>

#include "yb/common/wire_protocol.h"
#include "yb/fs/fs_manager.h"
#include "yb/gutil/strings/strcat.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/gutil/walltime.h"
#include "yb/rpc/messenger.h"
#include "yb/server/default-path-handlers.h"
#include "yb/server/generic_service.h"
#include "yb/server/glog_metrics.h"
#include "yb/server/hybrid_clock.h"
#include "yb/server/logical_clock.h"
#include "yb/server/rpc_server.h"
#include "yb/server/rpcz-path-handler.h"
#include "yb/server/server_base.pb.h"
#include "yb/server/server_base_options.h"
#include "yb/server/tcmalloc_metrics.h"
#include "yb/server/skewed_clock.h"
#include "yb/server/tracing-path-handlers.h"
#include "yb/server/webserver.h"
#include "yb/util/atomic.h"
#include "yb/util/env.h"
#include "yb/util/flag_tags.h"
#include "yb/util/jsonwriter.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/metrics.h"
#include "yb/util/monotime.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/net/net_util.h"
#include "yb/util/status.h"
#include "yb/util/user.h"
#include "yb/util/pb_util.h"
#include "yb/util/rolling_log.h"
#include "yb/util/spinlock_profiling.h"
#include "yb/util/thread.h"
#include "yb/util/version_info.h"
#include "yb/util/encryption_util.h"
#include "yb/gutil/sysinfo.h"

DEFINE_int32(num_reactor_threads, -1,
             "Number of libev reactor threads to start. If -1, the value is automatically set.");
TAG_FLAG(num_reactor_threads, advanced);

DECLARE_bool(use_hybrid_clock);

DEFINE_int32(generic_svc_num_threads, 10,
             "Number of RPC worker threads to run for the generic service");
TAG_FLAG(generic_svc_num_threads, advanced);

DEFINE_int32(generic_svc_queue_length, 50,
             "RPC Queue length for the generic service");
TAG_FLAG(generic_svc_queue_length, advanced);

DEFINE_string(yb_test_name, "",
              "Specifies test name this daemon is running as part of.");

DEFINE_bool(TEST_check_broadcast_address, true, "Break connectivity in test mini cluster to "
            "check broadcast address.");

DEFINE_test_flag(string, public_hostname_suffix, ".ip.yugabyte", "Suffix for public hostnames.");

DEFINE_test_flag(bool, simulate_port_conflict_error, false,
                 "Simulate a port conflict error during initialization.");

DEFINE_test_flag(int32, nodes_per_cloud, 2,
                 "Number of nodes per cloud to test private and public addresses.");

using namespace std::literals;
using namespace std::placeholders;

using std::shared_ptr;
using std::string;
using std::stringstream;
using std::vector;
using strings::Substitute;

namespace yb {
namespace server {

namespace {

// Disambiguates between servers when in a minicluster.
AtomicInt<int32_t> mem_tracker_id_counter(-1);

std::string kServerMemTrackerName = "server";

struct CommonMemTrackers {
  std::vector<MemTrackerPtr> trackers;

  ~CommonMemTrackers() {
#if defined(TCMALLOC_ENABLED)
    // Prevent root mem tracker from accessing common mem trackers.
    auto root = MemTracker::GetRootTracker();
    root->SetPollChildrenConsumptionFunctors(nullptr);
#endif
  }
};

std::unique_ptr<CommonMemTrackers> common_mem_trackers;

} // anonymous namespace

std::shared_ptr<MemTracker> CreateMemTrackerForServer() {
  int32_t id = mem_tracker_id_counter.Increment();
  std::string id_str = kServerMemTrackerName;
  if (id != 0) {
    StrAppend(&id_str, " ", id);
  }
  return MemTracker::CreateTracker(id_str);
}

#if defined(TCMALLOC_ENABLED)
void RegisterTCMallocTracker(const char* name, const char* prop) {
  common_mem_trackers->trackers.push_back(MemTracker::CreateTracker(
      -1, "TCMalloc "s + name, std::bind(&MemTracker::GetTCMallocProperty, prop)));
}
#endif

RpcServerBase::RpcServerBase(string name, const ServerBaseOptions& options,
                             const string& metric_namespace,
                             MemTrackerPtr mem_tracker)
    : name_(std::move(name)),
      mem_tracker_(std::move(mem_tracker)),
      metric_registry_(new MetricRegistry()),
      metric_entity_(METRIC_ENTITY_server.Instantiate(metric_registry_.get(),
                                                      metric_namespace)),
      options_(options),
      initialized_(false),
      stop_metrics_logging_latch_(1) {
  mem_tracker_->SetMetricEntity(metric_entity_);

#if defined(TCMALLOC_ENABLED)
  // When mem tracker for first server is created we register mem trackers that report tc malloc
  // status.
  if (mem_tracker_->id() == kServerMemTrackerName) {
    common_mem_trackers = std::make_unique<CommonMemTrackers>();

    RegisterTCMallocTracker("Thread Cache", "tcmalloc.thread_cache_free_bytes");
    RegisterTCMallocTracker("Central Cache", "tcmalloc.central_cache_free_bytes");
    RegisterTCMallocTracker("Transfer Cache", "tcmalloc.transfer_cache_free_bytes");
    RegisterTCMallocTracker("PageHeap Free", "tcmalloc.pageheap_free_bytes");

    auto root = MemTracker::GetRootTracker();
    root->SetPollChildrenConsumptionFunctors([]() {
          for (auto& tracker : common_mem_trackers->trackers) {
            tracker->UpdateConsumption();
          }
        });
  }
#endif

  if (FLAGS_use_hybrid_clock) {
    clock_ = new HybridClock();
  } else {
    clock_ = LogicalClock::CreateStartingAt(HybridTime::kInitial);
  }
}

void RpcServerBase::SetConnectionContextFactory(
    rpc::ConnectionContextFactoryPtr connection_context_factory) {
  rpc_server_.reset(new RpcServer(name_, options_.rpc_opts, std::move(connection_context_factory)));
}

RpcServerBase::~RpcServerBase() {
  Shutdown();
  rpc_server_.reset();
  messenger_.reset();
  if (mem_tracker_->parent()) {
    mem_tracker_->UnregisterFromParent();
  }
}

Endpoint RpcServerBase::first_rpc_address() const {
  const auto& addrs = rpc_server_->GetBoundAddresses();
  CHECK(!addrs.empty()) << "Not bound";
  return addrs[0];
}

const std::string RpcServerBase::get_hostname() const {
  auto hostname = GetHostname();
  if (hostname.ok()) {
    YB_LOG_FIRST_N(INFO, 1) << "Running on host: " << *hostname;
    return *hostname;
  } else {
    YB_LOG_FIRST_N(WARNING, 1) << "Failed to get current host name: " << hostname.status();
    return "unknown_hostname";
  }
}

const std::string RpcServerBase::get_current_user() const {
  string user_name;
  auto s = GetLoggedInUser(&user_name);
  if (s.ok()) {
    YB_LOG_FIRST_N(INFO, 1) << "Logged in user: " << user_name;
    return user_name;
  } else {
    YB_LOG_FIRST_N(WARNING, 1) << "Failed to get current user";
    return "unknown_user";
  }
}

const NodeInstancePB& RpcServerBase::instance_pb() const {
  return *DCHECK_NOTNULL(instance_pb_.get());
}

Status RpcServerBase::SetupMessengerBuilder(rpc::MessengerBuilder* builder) {
  if (FLAGS_num_reactor_threads == -1) {
    // Auto set the number of reactors based on the number of cores.
    FLAGS_num_reactor_threads =
        std::min(16, static_cast<int>(base::NumCPUs()));
    LOG(INFO) << "Auto setting FLAGS_num_reactor_threads to " << FLAGS_num_reactor_threads;
  }

  builder->set_num_reactors(FLAGS_num_reactor_threads);
  builder->set_metric_entity(metric_entity());
  builder->set_connection_keepalive_time(options_.rpc_opts.connection_keepalive_time_ms * 1ms);

  return Status::OK();
}

Status RpcServerBase::Init() {
  CHECK(!initialized_);

  glog_metrics_.reset(new ScopedGLogMetrics(metric_entity_));
  tcmalloc::RegisterMetrics(metric_entity_);
  RegisterSpinLockContentionMetrics(metric_entity_);

  InitSpinLockContentionProfiling();

  RETURN_NOT_OK(SetStackTraceSignal(SIGUSR2));

  // Initialize the clock immediately. This checks that the clock is synchronized
  // so we're less likely to get into a partially initialized state on disk during startup
  // if we're having clock problems.
  RETURN_NOT_OK_PREPEND(clock_->Init(), "Cannot initialize clock");

  // Create the Messenger.
  rpc::MessengerBuilder builder(name_);
  builder.UseDefaultConnectionContextFactory(mem_tracker());
  RETURN_NOT_OK(SetupMessengerBuilder(&builder));
  messenger_ = VERIFY_RESULT(builder.Build());
  proxy_cache_ = std::make_unique<rpc::ProxyCache>(messenger_.get());

  RETURN_NOT_OK(rpc_server_->Init(messenger_.get()));
  RETURN_NOT_OK(rpc_server_->Bind());
  clock_->RegisterMetrics(metric_entity_);

  RETURN_NOT_OK_PREPEND(StartMetricsLogging(), "Could not enable metrics logging");

  initialized_ = true;
  return Status::OK();
}

string RpcServerBase::ToString() const {
  return strings::Substitute("$0 : rpc=$1", name_, yb::ToString(first_rpc_address()));
}

void RpcServerBase::GetStatusPB(ServerStatusPB* status) const {
  // Node instance
  status->mutable_node_instance()->CopyFrom(*instance_pb_);

  // RPC ports
  {
    for (const auto& addr : rpc_server_->GetBoundAddresses()) {
      HostPortPB* pb = status->add_bound_rpc_addresses();
      pb->set_host(addr.address().to_string());
      pb->set_port(addr.port());
    }
  }

  VersionInfo::GetVersionInfoPB(status->mutable_version_info());
}

Status RpcServerBase::DumpServerInfo(const string& path,
                                     const string& format) const {
  ServerStatusPB status;
  GetStatusPB(&status);

  if (boost::iequals(format, "json")) {
    string json = JsonWriter::ToJson(status, JsonWriter::PRETTY);
    RETURN_NOT_OK(WriteStringToFile(options_.env, Slice(json), path));
  } else if (boost::iequals(format, "pb")) {
    // TODO: Use PB container format?
    RETURN_NOT_OK(pb_util::WritePBToPath(options_.env, path, status,
                                         pb_util::NO_SYNC)); // durability doesn't matter
  } else {
    return STATUS(InvalidArgument, "bad format", format);
  }

  LOG(INFO) << "Dumped server information to " << path;
  return Status::OK();
}

Status RpcServerBase::RegisterService(size_t queue_limit,
                                      rpc::ServiceIfPtr rpc_impl,
                                      rpc::ServicePriority priority) {
  return rpc_server_->RegisterService(queue_limit, std::move(rpc_impl), priority);
}

Status RpcServerBase::StartMetricsLogging() {
  if (options_.metrics_log_interval_ms <= 0) {
    return Status::OK();
  }

  return Thread::Create("server", "metrics-logger", &RpcAndWebServerBase::MetricsLoggingThread,
                        this, &metrics_logging_thread_);
}

void RpcServerBase::MetricsLoggingThread() {
  RollingLog log(Env::Default(), FLAGS_log_dir, "metrics");

  // How long to wait before trying again if we experience a failure
  // logging metrics.
  const MonoDelta kWaitBetweenFailures = MonoDelta::FromSeconds(60);

  MonoTime next_log = MonoTime::Now();
  while (!stop_metrics_logging_latch_.WaitUntil(next_log)) {
    next_log = MonoTime::Now();
    next_log.AddDelta(MonoDelta::FromMilliseconds(options_.metrics_log_interval_ms));

    std::stringstream buf;
    buf << "metrics " << GetCurrentTimeMicros() << " ";

    // Collect the metrics JSON string.
    vector<string> metrics;
    metrics.push_back("*");
    MetricJsonOptions opts;
    opts.include_raw_histograms = true;

    JsonWriter writer(&buf, JsonWriter::COMPACT);
    Status s = metric_registry_->WriteAsJson(&writer, metrics, opts);
    if (!s.ok()) {
      WARN_NOT_OK(s, "Unable to collect metrics to log");
      next_log.AddDelta(kWaitBetweenFailures);
      continue;
    }

    buf << "\n";

    s = log.Append(buf.str());
    if (!s.ok()) {
      WARN_NOT_OK(s, "Unable to write metrics to log");
      next_log.AddDelta(kWaitBetweenFailures);
      continue;
    }
  }

  WARN_NOT_OK(log.Close(), "Unable to close metric log");
}

Status RpcServerBase::Start() {
  std::unique_ptr<rpc::ServiceIf> gsvc_impl(new GenericServiceImpl(this));
  RETURN_NOT_OK(RegisterService(FLAGS_generic_svc_queue_length, std::move(gsvc_impl)));

  RETURN_NOT_OK(StartRpcServer());

  return Status::OK();
}

Status RpcServerBase::StartRpcServer() {
  CHECK(initialized_);

  RETURN_NOT_OK(rpc_server_->Start());

  if (!options_.dump_info_path.empty()) {
    RETURN_NOT_OK_PREPEND(DumpServerInfo(options_.dump_info_path, options_.dump_info_format),
                          "Failed to dump server info to " + options_.dump_info_path);
  }

  return Status::OK();
}

void RpcServerBase::Shutdown() {
  if (metrics_logging_thread_) {
    stop_metrics_logging_latch_.CountDown();
    metrics_logging_thread_->Join();
  }
  if (rpc_server_) {
    rpc_server_->Shutdown();
  }
  if (messenger_) {
    messenger_->Shutdown();
  }
}

RpcAndWebServerBase::RpcAndWebServerBase(
    string name, const ServerBaseOptions& options,
    const std::string& metric_namespace,
    MemTrackerPtr mem_tracker)
    : RpcServerBase(name, options, metric_namespace, std::move(mem_tracker)),
      web_server_(new Webserver(options.webserver_opts, name_)) {
  FsManagerOpts fs_opts;
  fs_opts.metric_entity = metric_entity_;
  fs_opts.parent_mem_tracker = mem_tracker_;
  fs_opts.wal_paths = options.fs_opts.wal_paths;
  fs_opts.data_paths = options.fs_opts.data_paths;
  fs_opts.server_type = options.server_type;
  fs_manager_.reset(new FsManager(options.env, fs_opts));

  CHECK_OK(StartThreadInstrumentation(metric_entity_, web_server_.get()));
}

RpcAndWebServerBase::~RpcAndWebServerBase() {
  Shutdown();
}

Endpoint RpcAndWebServerBase::first_http_address() const {
  std::vector<Endpoint> addrs;
  WARN_NOT_OK(web_server_->GetBoundAddresses(&addrs),
              "Couldn't get bound webserver addresses");
  CHECK(!addrs.empty()) << "Not bound";
  return addrs[0];
}

void RpcAndWebServerBase::GenerateInstanceID() {
  instance_pb_.reset(new NodeInstancePB);
  instance_pb_->set_permanent_uuid(fs_manager_->uuid());
  // TODO: maybe actually bump a sequence number on local disk instead of
  // using time.
  instance_pb_->set_instance_seqno(Env::Default()->NowMicros());
}

Status RpcAndWebServerBase::Init() {
  yb::enterprise::InitOpenSSL();

  Status s = fs_manager_->Open();
  if (s.IsNotFound() || (!s.ok() && fs_manager_->HasAnyLockFiles())) {
    LOG(INFO) << "Could not load existing FS layout: " << s.ToString();
    LOG(INFO) << "Creating new FS layout";
    RETURN_NOT_OK_PREPEND(fs_manager_->CreateInitialFileSystemLayout(true),
                          "Could not create new FS layout");
    s = fs_manager_->Open();
  }
  RETURN_NOT_OK_PREPEND(s, "Failed to load FS layout");

  if (PREDICT_FALSE(FLAGS_TEST_simulate_port_conflict_error)) {
    return STATUS(NetworkError, "Simulated port conflict error");
  }

  RETURN_NOT_OK(RpcServerBase::Init());

  return Status::OK();
}

void RpcAndWebServerBase::GetStatusPB(ServerStatusPB* status) const {
  RpcServerBase::GetStatusPB(status);

  // HTTP ports
  {
    std::vector<Endpoint> addrs;
    CHECK_OK(web_server_->GetBoundAddresses(&addrs));
    for (const auto& addr : addrs) {
      HostPortPB* pb = status->add_bound_http_addresses();
      pb->set_host(addr.address().to_string());
      pb->set_port(addr.port());
    }
  }
}

Status RpcAndWebServerBase::GetRegistration(ServerRegistrationPB* reg, RpcOnly rpc_only) const {
  std::vector<HostPort> addrs = CHECK_NOTNULL(rpc_server())->GetRpcHostPort();
  DCHECK_GE(addrs.size(), 1);

  // Fall back to hostname resolution if the rpc hostname is a wildcard.
  if (addrs.size() != 1 || IsWildcardAddress(addrs[0].host()) || addrs[0].port() == 0) {
    vector<Endpoint> endpoints =
        CHECK_NOTNULL(rpc_server())->GetBoundAddresses();
    RETURN_NOT_OK_PREPEND(
        AddHostPortPBs(endpoints, reg->mutable_private_rpc_addresses()),
        "Failed to add RPC endpoints to registration");
    for (const auto &addr : reg->private_rpc_addresses()) {
      LOG(INFO) << " Using private rpc addresses: ( " << addr.ShortDebugString()
                << " )";
    }
  } else {
    HostPortsToPBs(addrs, reg->mutable_private_rpc_addresses());
    LOG(INFO) << "Using private rpc address "
              << reg->private_rpc_addresses(0).host();
  }

  HostPortsToPBs(options_.broadcast_addresses, reg->mutable_broadcast_addresses());

  if (!rpc_only) {
    HostPort web_input_hp;
    RETURN_NOT_OK(CHECK_NOTNULL(web_server())->GetInputHostPort(&web_input_hp));
    if (IsWildcardAddress(web_input_hp.host()) || web_input_hp.port() == 0) {
      std::vector<Endpoint> web_addrs;
      RETURN_NOT_OK_PREPEND(
          CHECK_NOTNULL(web_server())->GetBoundAddresses(&web_addrs),
          "Unable to get bound HTTP addresses");
      RETURN_NOT_OK_PREPEND(AddHostPortPBs(
          web_addrs, reg->mutable_http_addresses()),
          "Failed to add HTTP addresses to registration");
      for (const auto &addr : reg->http_addresses()) {
        LOG(INFO) << "Using http addresses: ( " << addr.ShortDebugString() << " )";
      }
    } else {
      HostPortsToPBs({ web_input_hp }, reg->mutable_http_addresses());
      LOG(INFO) << "Using http address " << reg->http_addresses(0).host();
    }
  }
  reg->mutable_cloud_info()->set_placement_cloud(options_.placement_cloud());
  reg->mutable_cloud_info()->set_placement_region(options_.placement_region());
  reg->mutable_cloud_info()->set_placement_zone(options_.placement_zone());
  reg->set_placement_uuid(options_.placement_uuid);
  return Status::OK();
}

string RpcAndWebServerBase::GetEasterEggMessage() const {
  return "Congratulations on installing YugabyteDB. "
         "We'd like to welcome you to the community with a free t-shirt and pack of stickers! "
         "Please claim your reward here: <a href='https://www.yugabyte.com/community-rewards/'>"
         "https://www.yugabyte.com/community-rewards/</a>";

}

string RpcAndWebServerBase::FooterHtml() const {
  return Substitute("<pre class='message'><i class=\"fa-lg fa fa-gift\" aria-hidden=\"true\"></i>"
                    " $0</pre><pre>$1\nserver uuid $2</pre>",
                    GetEasterEggMessage(),
                    VersionInfo::GetShortVersionString(),
                    instance_pb_->permanent_uuid());
}

void RpcAndWebServerBase::DisplayIconTile(std::stringstream* output, const string icon,
                                          const string caption, const string url) {
  *output << "  <div class='col-sm-4 col-md-4 dbg-tile'>\n"
          << "    <a href='" << url << "' class='thumbnail'>\n"
          << "      <div class='dbg-icon'>\n"
          << "        <i class='fa " << icon << "' aria-hidden='true'></i>\n"
          << "      </div>\n"
          << "      <div class='caption dbg-caption'>\n"
          << "        <h3>" << caption << "</h3>\n"
          << "      </div> <!-- caption -->\n"
          << "    </a> <!-- thumbnail -->\n"
          << "  </div> <!-- col-sm-4 col-md-4 -->\n";
}

void RpcAndWebServerBase::DisplayGeneralInfoIcons(std::stringstream* output) {
  // Logs.
  DisplayIconTile(output, "fa-files-o", "Logs", "/logs");
  // GFlags.
  DisplayIconTile(output, "fa-flag-o", "GFlags", "/varz");
  // Memory trackers.
  DisplayIconTile(output, "fa-bar-chart", "Memory Breakdown", "/mem-trackers");
  // Total memory.
  DisplayIconTile(output, "fa-cog", "Total Memory", "/memz");
  // Metrics.
  DisplayIconTile(output, "fa-line-chart", "Metrics", "/metrics");
  // Threads.
  DisplayIconTile(output, "fa-microchip", "Threads", "/threadz");
}

Status RpcAndWebServerBase::DisplayRpcIcons(std::stringstream* output) {
  // RPCs in Progress.
  DisplayIconTile(output, "fa-tasks", "Server RPCs", "/rpcz");
  return Status::OK();
}

Status RpcAndWebServerBase::HandleDebugPage(const Webserver::WebRequest& req,
                                            Webserver::WebResponse* resp) {
  std::stringstream *output = &resp->output;
  *output << "<h1>Debug Utilities</h1>\n";

  *output << "<div class='row debug-tiles'>\n";
  *output << "<h2> General Info </h2>";
  DisplayGeneralInfoIcons(output);
  *output << "</div> <!-- row -->\n";
  *output << "<h2> RPCs In Progress </h2>";
  *output << "<div class='row debug-tiles'>\n";
  RETURN_NOT_OK(DisplayRpcIcons(output));
  *output << "</div> <!-- row -->\n";
  return Status::OK();
}

Status RpcAndWebServerBase::Start() {
  GenerateInstanceID();

  AddDefaultPathHandlers(web_server_.get());
  AddRpczPathHandlers(messenger_.get(), web_server_.get());
  RegisterMetricsJsonHandler(web_server_.get(), metric_registry_.get());
  TracingPathHandlers::RegisterHandlers(web_server_.get());
  web_server_->RegisterPathHandler("/utilz", "Utilities",
                                   std::bind(&RpcAndWebServerBase::HandleDebugPage, this, _1, _2),
                                   true, true, "fa fa-wrench");
  web_server_->set_footer_html(FooterHtml());
  RETURN_NOT_OK(web_server_->Start());

  RETURN_NOT_OK(RpcServerBase::Start());

  return Status::OK();
}

void RpcAndWebServerBase::Shutdown() {
  RpcServerBase::Shutdown();
  web_server_->Stop();
}

std::string TEST_RpcAddress(int index, Private priv) {
  return Format("127.0.0.$0$1",
                index * 2 + (priv ? 0 : 1), priv ? "" : FLAGS_TEST_public_hostname_suffix);
}

string TEST_RpcBindEndpoint(int index, uint16_t port) {
  return HostPortToString(TEST_RpcAddress(index, Private::kTrue), port);
}

constexpr int kMaxServers = 20;
constexpr int kMinServerIdx = 1;

// We group servers by two. Servers in the same group communciate via private connection. Servers in
// different groups communicate via public connection.
int ServerGroupNum(int server_idx) {
  return (server_idx - 1) / FLAGS_TEST_nodes_per_cloud;
}

void TEST_SetupConnectivity(rpc::Messenger* messenger, int index) {
  if (!FLAGS_TEST_check_broadcast_address) {
    return;
  }

  CHECK_GE(index, kMinServerIdx);
  CHECK_LE(index, kMaxServers);

  messenger->TEST_SetOutboundIpBase(
      CHECK_RESULT(HostToAddress(TEST_RpcAddress(index, Private::kTrue))));
  auto server_group = ServerGroupNum(index);
  for (int other_server_idx = kMinServerIdx; other_server_idx <= kMaxServers; ++other_server_idx) {
    // We group servers by 2. When servers belongs to the same group, they should use
    // private ip for communication, otherwise public ip should be used.
    bool same_group = ServerGroupNum(other_server_idx) == server_group;
    auto broken_address = CHECK_RESULT(
        HostToAddress(TEST_RpcAddress(other_server_idx, Private(!same_group))));
    LOG(INFO) << "Break " << index << " => " << broken_address;
    messenger->BreakConnectivityWith(broken_address);
    auto working_address = CHECK_RESULT(
        HostToAddress(TEST_RpcAddress(other_server_idx, Private(same_group))));
    messenger->RestoreConnectivityWith(working_address);
  }
}

void TEST_Isolate(rpc::Messenger* messenger) {
  for (int other_server_idx = kMinServerIdx; other_server_idx <= kMaxServers; ++other_server_idx) {
    messenger->BreakConnectivityWith(
        CHECK_RESULT(HostToAddress(TEST_RpcAddress(other_server_idx, Private::kTrue))));
    messenger->BreakConnectivityWith(
        CHECK_RESULT(HostToAddress(TEST_RpcAddress(other_server_idx, Private::kFalse))));
  }
}

} // namespace server
} // namespace yb
