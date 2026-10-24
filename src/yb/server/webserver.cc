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
// The following only applies to changes made to this file as part of YugabyteDB development.
//
// Portions Copyright (c) YugabyteDB, Inc.
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
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "yb/server/webserver.h"

#include <stdio.h>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/algorithm/string.hpp>
#include <squeasel.h>

#include "yb/gutil/dynamic_annotations.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/stl_util.h"
#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/numbers.h"
#include "yb/gutil/strings/split.h"
#include "yb/gutil/strings/stringpiece.h"
#include "yb/gutil/strings/strip.h"

#include "yb/util/env.h"
#include "yb/util/flags.h"
#include "yb/util/logging.h"
#include "yb/util/mem_tracker.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/scope_exit.h"
#include "yb/util/shared_lock.h"
#include "yb/util/status.h"
#include "yb/util/status_format.h"
#include "yb/util/tcmalloc_util.h"
#include "yb/util/url-coding.h"
#include "yb/util/zlib.h"

#if defined(__APPLE__)
typedef sig_t sighandler_t;
#endif

DEFINE_RUNTIME_int32(webserver_max_post_length_bytes, 1024 * 1024,
    "The maximum length of a POST request that will be accepted by "
    "the embedded web server.");
TAG_FLAG(webserver_max_post_length_bytes, advanced);

DEFINE_RUNTIME_int32(webserver_zlib_compression_level, 1,
    "The zlib compression level."
    "Lower compression levels result in faster execution, but less compression");
TAG_FLAG(webserver_zlib_compression_level, advanced);

DEFINE_RUNTIME_uint64(webserver_compression_threshold_kb, 4,
    "The threshold of response size above which compression is performed."
    "Default value is 4KB");
TAG_FLAG(webserver_compression_threshold_kb, advanced);

DEFINE_UNKNOWN_bool(webserver_redirect_http_to_https, false,
            "Redirect HTTP requests to the embedded webserver to HTTPS if HTTPS is enabled.");

DEFINE_RUNTIME_bool(webserver_strict_transport_security, false,
    "Header is cached by the browser for the specified 'max-age' and forces it to redirect any "
    "http request to https to avoid man-in-the-middle attacks. The original http request is never "
    "sent.");

// This flag is used to give different internal mini cluster TServers different values of flags like
// --tserver_master_addrs at least as far as seen via the /varz endpoint; this is necessary because
// yb-controller uses this endpoint to figure out information on how to contact TServers and
// masters.
DEFINE_test_flag(bool, use_custom_varz, false,
    "Cause /varz endpoint of TServers to output for selected gflags their values at "
    "startup rather than their current values.");

namespace yb {

using std::string;
using std::stringstream;
using std::vector;
using std::make_pair;

using namespace std::placeholders;

class Webserver::Impl {
 public:
  Impl(const WebserverOptions& opts, const std::string& server_name);

  ~Impl();

  Status Start();

  void Stop();

  Status GetBoundAddresses(std::vector<Endpoint>* addrs) const;

  Status GetInputHostPort(HostPort* hp) const;

  void RegisterPathHandler(const std::string& path, const std::string& alias,
                                   const PathHandlerCallback& callback,
                                   bool is_styled = true,
                                   bool is_on_nav_bar = true,
                                   const std::string icon = "");

  void set_footer_html(const std::string& html);

  bool IsSecure() const;

  void SetAutoFlags(std::unordered_set<std::string>&& flags) EXCLUDES(flags_mutex_);

  bool ContainsAutoFlag(const std::string& flag) const EXCLUDES(flags_mutex_);

  void SetFlags(std::unordered_set<std::string>&& flags) EXCLUDES(flags_mutex_);

  bool ContainsFlag(const std::string& flag) const EXCLUDES(flags_mutex_);

  void SetLogging(bool enable_access_logging, bool enable_tcmalloc_logging) {
    access_logging_enabled_.store(enable_access_logging, std::memory_order_release);
    tcmalloc_logging_enabled_.store(enable_tcmalloc_logging, std::memory_order_release);
  }

  const std::string& DocRoot() const { return opts_.doc_root; }

 private:
  // Container class for a list of path handler callbacks for a single URL.
  class PathHandler {
   public:
    PathHandler(bool is_styled, bool is_on_nav_bar, std::string alias, const std::string icon)
        : is_styled_(is_styled),
          is_on_nav_bar_(is_on_nav_bar),
          alias_(std::move(alias)),
          icon_(icon) {}

    void AddCallback(const PathHandlerCallback& callback) {
      callbacks_.push_back(callback);
    }

    bool is_styled() const { return is_styled_; }
    bool is_on_nav_bar() const { return is_on_nav_bar_; }
    const std::string& alias() const { return alias_; }
    const std::string& icon() const { return icon_; }
    const std::vector<PathHandlerCallback>& callbacks() const { return callbacks_; }

   private:
    // If true, the page appears is rendered styled.
    bool is_styled_;

    // If true, the page appears in the navigation bar.
    bool is_on_nav_bar_;

    // Alias used when displaying this link on the nav bar.
    std::string alias_;

    // Icon used when displaying this link on the nav bar.
    std::string icon_;

    // List of callbacks to render output for this page, called in order.
    std::vector<PathHandlerCallback> callbacks_;
  };

  bool static_pages_available() const;

  // Build the string to pass to mongoose specifying where to bind.
  Status BuildListenSpec(std::string* spec) const;

  // Renders a common Bootstrap-styled header
  void BootstrapPageHeader(std::stringstream* output);

  // Renders a common Bootstrap-styled footer. Must be used in conjunction with
  // BootstrapPageHeader.
  void BootstrapPageFooter(std::stringstream* output);

  // Dispatch point for all incoming requests.
  // Static so that it can act as a function pointer, and then call the next method
  static sq_callback_result_t BeginRequestCallbackStatic(struct sq_connection* connection);
  sq_callback_result_t BeginRequestCallback(struct sq_connection* connection,
                                            struct sq_request_info* request_info);

  sq_callback_result_t RunPathHandler(const PathHandler& handler,
                                      struct sq_connection* connection,
                                      struct sq_request_info* request_info);

  // Callback to funnel mongoose logs through glog.
  static int LogMessageCallbackStatic(const struct sq_connection* connection,
                                      const char* message);

  // Registered to handle "/", and prints a list of available URIs
  void RootHandler(const WebRequest& args, Webserver::WebResponse* resp);

  // Builds a map of argument name to argument value from a typical URL argument
  // string (that is, "key1=value1&key2=value2.."). If no value is given for a
  // key, it is entered into the map as (key, "").
  void BuildArgumentMap(const std::string& args, ArgumentMap* output);

  const WebserverOptions opts_;

  // Lock guarding the path_handlers_ map and footer_html.
  std::shared_timed_mutex lock_;

  // Map of path to a PathHandler containing a list of handlers for that
  // path. More than one handler may register itself with a path so that many
  // components may contribute to a single page.
  typedef std::map<std::string, PathHandler*> PathHandlerMap;
  PathHandlerMap path_handlers_;

  // Snippet of HTML which will be displayed in the footer of all pages
  // rendered by this server. Protected by 'lock_'.
  std::string footer_html_;

  // The address of the interface on which to run this webserver.
  std::string http_address_;

  // Handle to Mongoose context; owned and freed by Mongoose internally
  struct sq_context* context_;

  // Server name for display purposes
  std::string server_name_;

  // Mutex guarding against concurrenct calls to Stop().
  std::mutex stop_mutex_;

  // Variable to notify handlers to stop serving requests.
  std::atomic<bool> stop_initiated = false;

  mutable std::mutex flags_mutex_;
  // The flags that are associated with this particular server. In LTO builds we use the same
  // process for both yb-master and yb-tserver, so the running process may have more flags than
  // this server needs. This is used to filter out the flags that are shown in the varz UI.
  std::unordered_set<std::string> auto_flags_ GUARDED_BY(flags_mutex_);
  std::unordered_set<std::string> process_flags_ GUARDED_BY(flags_mutex_);

  std::atomic<bool> access_logging_enabled_ = false;
  std::atomic<bool> tcmalloc_logging_enabled_ = false;
};

Webserver::Impl::Impl(const WebserverOptions& opts, const std::string& server_name)
  : opts_(opts),
    context_(nullptr),
    server_name_(server_name) {
  string host = opts.bind_interface.empty() ? "0.0.0.0" : opts.bind_interface;
  http_address_ = host + ":" + std::to_string(opts.port);
}

Webserver::Impl::~Impl() {
  Stop();
  STLDeleteValues(&path_handlers_);
}

void Webserver::Impl::RootHandler(const Webserver::WebRequest& args, Webserver::WebResponse* resp) {
}

void Webserver::Impl::BuildArgumentMap(const string& args, ArgumentMap* output) {
  vector<GStringPiece> arg_pairs = strings::Split(args, "&");

  for (const GStringPiece& arg_pair : arg_pairs) {
    vector<GStringPiece> key_value = strings::Split(arg_pair, "=");
    if (key_value.empty()) continue;

    string key;
    if (!UrlDecode(key_value[0].ToString(), &key)) continue;
    string value;
    if (!UrlDecode((key_value.size() >= 2 ? key_value[1].ToString() : ""), &value)) continue;
    boost::to_lower(key);
    (*output)[key] = value;
  }
}

bool Webserver::Impl::IsSecure() const {
  return !opts_.certificate_file.empty();
}

void Webserver::Impl::SetAutoFlags(std::unordered_set<std::string>&& flags) {
  std::lock_guard l(flags_mutex_);
  auto_flags_ = std::move(flags);
}

bool Webserver::Impl::ContainsAutoFlag(const std::string& flag) const {
  std::lock_guard l(flags_mutex_);
  return auto_flags_.contains(flag);
}

void Webserver::Impl::SetFlags(std::unordered_set<std::string>&& flags) {
  std::lock_guard l(flags_mutex_);
  process_flags_ = std::move(flags);
}

bool Webserver::Impl::ContainsFlag(const std::string& flag) const {
  std::lock_guard l(flags_mutex_);
  return process_flags_.contains(flag);
}

Status Webserver::Impl::BuildListenSpec(string* spec) const {
  std::vector<Endpoint> endpoints;
  RETURN_NOT_OK(ParseAddressList(http_address_, 80, &endpoints));
  if (endpoints.empty()) {
    return STATUS_FORMAT(
      ConfigurationError,
      "No IPs available for address $0", http_address_);
  }
  std::vector<string> parts;
  std::string suffix;
  if (IsSecure()) {
    // Sockets with 's' suffix accept SSL traffic.
    // Sockets with 'r' suffix redirects non-SSL traffic to a SSL socket.
    suffix = FLAGS_webserver_redirect_http_to_https ? "rs" : "s";
  }
  for (const auto& endpoint : endpoints) {
    parts.push_back(ToString(endpoint) + suffix);
  }

  JoinStrings(parts, ",", spec);
  LOG(INFO) << "Webserver listen spec is " << *spec;
  return Status::OK();
}

Status Webserver::Impl::Start() {
  LOG(INFO) << "Starting webserver on " << http_address_;

  // Mini-cluster tests can restart the webserver by calling Stop() and Start() on the same instance
  // of the webserver. To support this, reset the state that was set during the previous shutdown.
  stop_initiated = false;

  vector<const char*> options;

  if (static_pages_available()) {
    LOG(INFO) << "Document root: " << opts_.doc_root;
    options.push_back("document_root");
    options.push_back(opts_.doc_root.c_str());
    options.push_back("enable_directory_listing");
    options.push_back("no");
  } else {
    LOG(INFO)<< "Document root disabled";
  }

  if (IsSecure()) {
    LOG(INFO) << "Webserver: Enabling HTTPS support";

    options.push_back("ssl_certificate");
    options.push_back(opts_.certificate_file.c_str());

    if (opts_.private_key_file.c_str()) {
      options.push_back("ssl_private_key");
      options.push_back(opts_.private_key_file.c_str());
    }

    if (opts_.private_key_password.c_str()) {
      options.push_back("ssl_private_key_password");
      options.push_back(opts_.private_key_password.c_str());
    }

    // We already initialize OpenSSL, so no need for Squeasel to do it.
    options.push_back("ssl_global_init");
    options.push_back("no");

    if (opts_.ssl_ciphers.c_str()) {
      LOG(INFO) << "Webserver: Setting ssl_ciphers to " << opts_.ssl_ciphers;
      options.push_back("ssl_ciphers");
      options.push_back(opts_.ssl_ciphers.c_str());
    }

    if (opts_.ssl_min_version.c_str()) {
      LOG(INFO) << "Webserver: Setting ssl_min_version to " << opts_.ssl_min_version;
      options.push_back("ssl_min_version");
      options.push_back(opts_.ssl_min_version.c_str());
    }
  }

  if (!opts_.authentication_domain.empty()) {
    options.push_back("authentication_domain");
    options.push_back(opts_.authentication_domain.c_str());
  }

  if (!opts_.password_file.empty()) {
    // Mongoose doesn't log anything if it can't stat the password file (but will if it
    // can't open it, which it tries to do during a request)
    if (!Env::Default()->FileExists(opts_.password_file)) {
      stringstream ss;
      ss << "Webserver: Password file does not exist: " << opts_.password_file;
      return STATUS(InvalidArgument, ss.str());
    }
    LOG(INFO) << "Webserver: Password file is " << opts_.password_file;
    // The bundled squeasel registers this config option under the name "global_auth_file"
    // (the GLOBAL_PASSWORDS_FILE enum is only the internal index). Passing the old Mongoose/Kudu
    // name "global_passwords_file" makes sq_start() reject it as an invalid option and refuse to
    // start the webserver, breaking --webserver_password_file authentication entirely.
    options.push_back("global_auth_file");
    options.push_back(opts_.password_file.c_str());
  }

  options.push_back("listening_ports");
  string listening_str;
  RETURN_NOT_OK(BuildListenSpec(&listening_str));
  options.push_back(listening_str.c_str());

  // Num threads
  options.push_back("num_threads");
  string num_threads_str = SimpleItoa(opts_.num_worker_threads);
  options.push_back(num_threads_str.c_str());

  // Options must be a NULL-terminated list
  options.push_back(nullptr);

  // mongoose ignores SIGCHLD and we need it to run kinit. This means that since
  // mongoose does not reap its own children CGI programs must be avoided.
  // Save the signal handler so we can restore it after mongoose sets it to be ignored.
  sighandler_t sig_chld = signal(SIGCHLD, SIG_DFL);

  sq_callbacks callbacks;
  memset(&callbacks, 0, sizeof(callbacks));
  callbacks.begin_request = &Webserver::Impl::BeginRequestCallbackStatic;
  callbacks.log_message = &Webserver::Impl::LogMessageCallbackStatic;

  // To work around not being able to pass member functions as C callbacks, we store a
  // pointer to this server in the per-server state, and register a static method as the
  // default callback. That method unpacks the pointer to this and calls the real
  // callback.
  context_ = sq_start(&callbacks, reinterpret_cast<void*>(this), &options[0]);

  // Restore the child signal handler so wait() works properly.
  signal(SIGCHLD, sig_chld);

  if (context_ == nullptr) {
    stringstream error_msg;
    error_msg << "Webserver: Could not start on address " << http_address_;
    TryRunLsof(Endpoint(IpAddress(), opts_.port));
    return STATUS(NetworkError, error_msg.str());
  }

  PathHandlerCallback default_callback =
      std::bind(boost::mem_fn(&Webserver::Impl::RootHandler), this, _1, _2);

  RegisterPathHandler("/", "Home", default_callback);

  std::vector<Endpoint> addrs;
  RETURN_NOT_OK(GetBoundAddresses(&addrs));
  string bound_addresses_str;
  string protocol = IsSecure() ? "https://" : "http://";
  for (const auto& addr : addrs) {
    if (!bound_addresses_str.empty()) {
      bound_addresses_str += ", ";
    }

    bound_addresses_str += protocol + ToString(addr) + "/";
  }

  LOG(INFO) << "Webserver started. Bound to: " << bound_addresses_str;
  return Status::OK();
}

void Webserver::Impl::Stop() {
  // Indicate to the handlers that they can now stop serving requests. If any further signals are
  // received to terminate the webserver, the handlers will no longer access non-static variables
  // and will return immediately.
  stop_initiated = true;

  std::lock_guard lock_(stop_mutex_);
  if (context_ != nullptr) {
    LOG(INFO) << "Stopping webserver on " << http_address_;
    sq_stop(context_);
    context_ = nullptr;
  }

  LOG(INFO) << "Webserver stopped";
}

Status Webserver::Impl::GetInputHostPort(HostPort* hp) const {
  std::vector<HostPort> parsed_hps;
  RETURN_NOT_OK(HostPort::ParseStrings(
    http_address_,
    0 /* default port */,
    &parsed_hps));

  // Webserver always gets a single host:port specification from WebserverOptions.
  DCHECK_EQ(parsed_hps.size(), 1);
  if (parsed_hps.size() != 1) {
    return STATUS(InvalidArgument, "Expected single host port in WebserverOptions host port");
  }

  *hp = parsed_hps[0];
  return Status::OK();
}

Status Webserver::Impl::GetBoundAddresses(std::vector<Endpoint>* addrs_ptr) const {
  if (!context_) {
    return STATUS(IllegalState, "Not started");
  }

  struct sockaddr_storage** sockaddrs = nullptr;
  int num_addrs = 0;

  if (sq_get_bound_addresses(context_, &sockaddrs, &num_addrs)) {
    return STATUS(NetworkError, "Unable to get bound addresses from Mongoose");
  }

  if (!num_addrs) {
    return Status::OK();
  }
  DCHECK(sockaddrs);

  ScopeExit cleanup{
    [sockaddrs, num_addrs] {
      for (int i = 0; i < num_addrs; ++i) {
        free(sockaddrs[i]);
      }
      free(sockaddrs);
    }};

  auto& addrs = *addrs_ptr;
  addrs.resize(num_addrs);

  for (int i = 0; i < num_addrs; i++) {
    switch (sockaddrs[i]->ss_family) {
      case AF_INET: {
        sockaddr_in* addr = reinterpret_cast<struct sockaddr_in*>(sockaddrs[i]);
        RSTATUS_DCHECK(
            addrs[i].capacity() >= sizeof(*addr), IllegalState, "Unexpected size of struct");
        memcpy(addrs[i].data(), addr, sizeof(*addr));
        break;
      }
      case AF_INET6: {
        sockaddr_in6* addr6 = reinterpret_cast<struct sockaddr_in6*>(sockaddrs[i]);
        RSTATUS_DCHECK(
            addrs[i].capacity() >= sizeof(*addr6), IllegalState, "Unexpected size of struct");
        memcpy(addrs[i].data(), addr6, sizeof(*addr6));
        break;
      }
      default: {
        LOG(DFATAL) << "Unexpected address family: " << sockaddrs[i]->ss_family;
        RSTATUS_DCHECK(false, IllegalState, "Unexpected address family");
        break;
      }
    }
  }

  return Status::OK();
}

int Webserver::Impl::LogMessageCallbackStatic(const struct sq_connection* connection,
                                              const char* message) {
  if (message != nullptr) {
    LOG(INFO) << "Webserver: " << message;
    return 1;
  }
  return 0;
}

sq_callback_result_t Webserver::Impl::BeginRequestCallbackStatic(struct sq_connection* connection) {
  struct sq_request_info* request_info = sq_get_request_info(connection);
  Webserver::Impl* instance = reinterpret_cast<Webserver::Impl*>(request_info->user_data);
  return instance->BeginRequestCallback(connection, request_info);
}

sq_callback_result_t Webserver::Impl::BeginRequestCallback(struct sq_connection* connection,
                                                           struct sq_request_info* request_info) {
  PathHandler* handler;
  {
    SharedLock<std::shared_timed_mutex> lock(lock_);
    PathHandlerMap::const_iterator it = path_handlers_.find(request_info->uri);
    if (it == path_handlers_.end()) {
      // Let Mongoose deal with this request; returning NULL will fall through
      // to the default handler which will serve files.
      if (static_pages_available()) {
        VLOG(2) << "HTTP File access: " << request_info->uri;
        return SQ_CONTINUE_HANDLING;
      } else {
        sq_printf(connection, "HTTP/1.1 404 Not Found\r\n"
                  "Content-Type: text/plain\r\n\r\n");
        sq_printf(connection, "No handler for URI %s\r\n\r\n", request_info->uri);
        return SQ_HANDLED_OK;
      }
    }
    handler = it->second;
  }

  if (access_logging_enabled_.load(std::memory_order_acquire)) {
    string params = request_info->query_string ? Format("?$0", request_info->query_string) : "";
    LOG(INFO) << "webserver request: " << request_info->uri << params;
  }

  sq_callback_result_t result = RunPathHandler(*handler, connection, request_info);
  MemTracker::GcTcmallocIfNeeded();

#if YB_TCMALLOC_ENABLED
  if (tcmalloc_logging_enabled_.load(std::memory_order_acquire))
    LOG(INFO) << "webserver tcmalloc stats:"
              << " heap size bytes: " << GetTCMallocPhysicalBytesUsed()
              << ", total physical bytes: " << GetTCMallocCurrentHeapSizeBytes()
              << ", current allocated bytes: " << GetTCMallocCurrentAllocatedBytes()
              << ", page heap free bytes: " << GetTCMallocPageHeapFreeBytes()
              << ", page heap unmapped bytes: " << GetTCMallocPageHeapUnmappedBytes();
#endif

  return result;
}

sq_callback_result_t Webserver::Impl::RunPathHandler(const PathHandler& handler,
                                                     struct sq_connection* connection,
                                                     struct sq_request_info* request_info) {
  constexpr auto SERVICE_UNAVAILABLE_MSG = "HTTP/1.1 503 Service Unavailable\r\n\r\n";
  // If we're in the process of stopping, do not parse or route the request. Just return.
  if (PREDICT_FALSE(stop_initiated)) {
    sq_printf(connection, SERVICE_UNAVAILABLE_MSG);
    return SQ_HANDLED_CLOSE_CONNECTION;
  }

  // Should we render with css styles?
  bool use_style = true;

  WebRequest req;
  req.redirect_uri = request_info->uri;
  if (request_info->query_string != nullptr) {
    req.query_string = request_info->query_string;
    BuildArgumentMap(request_info->query_string, &req.parsed_args);
  }

  if (FLAGS_TEST_use_custom_varz) {
    // Pass custom G-flags into the request handler.
    req.parsed_args["TEST_custom_varz"] = opts_.TEST_custom_varz;
  }

  req.request_method = request_info->request_method;
  if (req.request_method == "POST") {
    const char* content_len_str = sq_get_header(connection, "Content-Length");
    int32_t content_len = 0;
    if (content_len_str == nullptr ||
        !safe_strto32(content_len_str, &content_len)) {
      sq_printf(connection, "HTTP/1.1 411 Length Required\r\n");
      return SQ_HANDLED_OK;
    }
    if (content_len > FLAGS_webserver_max_post_length_bytes) {
      // TODO: for this and other HTTP requests, we should log the
      // remote IP, etc.
      LOG(WARNING) << "Rejected POST with content length " << content_len;
      sq_printf(connection, "HTTP/1.1 413 Request Entity Too Large\r\n");
      return SQ_HANDLED_CLOSE_CONNECTION;
    }

    char buf[8192];
    int rem = content_len;
    while (rem > 0) {
      int n = sq_read(connection, buf, std::min<int>(sizeof(buf), rem));
      if (n <= 0) {
        LOG(WARNING) << "error reading POST data: expected "
                     << content_len << " bytes but only read "
                     << req.post_data.size();
        sq_printf(connection, "HTTP/1.1 500 Internal Server Error\r\n");
        return SQ_HANDLED_CLOSE_CONNECTION;
      }

      req.post_data.append(buf, n);
      rem -= n;
    }
  }

  if (!handler.is_styled() || ContainsKey(req.parsed_args, "raw")) {
    use_style = false;
  }

  WebResponse resp;
  WebResponse* resp_ptr = &resp;
  // Default response code should be OK.
  resp_ptr->code = 200;
  stringstream *output = &resp_ptr->output;
  if (use_style) {
    BootstrapPageHeader(output);
  }
  for (const PathHandlerCallback& callback_ : handler.callbacks()) {
    callback_(req, resp_ptr);
    if (resp_ptr->code == 503) {
      sq_printf(connection, SERVICE_UNAVAILABLE_MSG);
      return SQ_HANDLED_CLOSE_CONNECTION;
    }
  }
  if (use_style) {
    BootstrapPageFooter(output);
  }
  // Check if gzip compression is accepted by the caller. If so, compress the
  // content and replace the prerendered output.
  const char* accept_encoding_str = sq_get_header(connection, "Accept-Encoding");
  bool is_compressed = false;
  vector<string> encodings = strings::Split(accept_encoding_str, ",");
  for (string& encoding : encodings) {
    StripWhiteSpace(&encoding);
    if (encoding == "gzip") {
      // Don't bother compressing empty content.
      const string& uncompressed = resp_ptr->output.str();
      if (uncompressed.size() < FLAGS_webserver_compression_threshold_kb * 1024) {
        break;
      }

      std::ostringstream oss;
      int level = FLAGS_webserver_zlib_compression_level > 0 &&
        FLAGS_webserver_zlib_compression_level <= 9 ?
        FLAGS_webserver_zlib_compression_level : 1;
      Status s = zlib::CompressLevel(uncompressed, level, &oss);
      if (s.ok()) {
        resp_ptr->output.str(oss.str());
        is_compressed = true;
      } else {
        LOG(WARNING) << "Could not compress output: " << s.ToString();
      }
      break;
    }
  }

  // Generating headers and response.
  string str = output->str();
  auto content_type = use_style ? "text/html" : "text/plain";
  auto content_encoding = is_compressed ? "Content-Encoding: gzip\r\n" : "";
  auto hsts = IsSecure() && FLAGS_webserver_strict_transport_security
                  ? "Strict-Transport-Security: max-age=31536000\r\n"
                  : "";

  sq_printf(
      connection,
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zd\r\n"
      // Squeasel is not configured for keep-alive and closes the socket once the handler
      // returns. Without this header the response looks persistent (HTTP/1.1 default), so a
      // client may reuse a connection that is already being closed and lose the next request.
      "Connection: close\r\n"
      "X-Content-Type-Options: nosniff\r\n"
      "%s"
      "%s"
      "Access-Control-Allow-Origin: *\r\n"
      "\r\n",
      content_type, str.length(), content_encoding, hsts);

  // Make sure to use sq_write for printing the body; sq_printf truncates at 8kb
  sq_write(connection, str.c_str(), str.length());
  return SQ_HANDLED_OK;
}

void Webserver::Impl::RegisterPathHandler(const string& path,
                                          const string& alias,
                                          const PathHandlerCallback& callback,
                                          bool is_styled,
                                          bool is_on_nav_bar,
                                          const std::string icon) {
  std::lock_guard lock(lock_);
  auto it = path_handlers_.find(path);
  if (it == path_handlers_.end()) {
    it = path_handlers_.insert(
        make_pair(path, new PathHandler(is_styled, is_on_nav_bar, alias, icon))).first;
  }
  it->second->AddCallback(callback);
}

const char* const PAGE_HEADER = "<!DOCTYPE html>"
"<html>"
"  <head>"
"    <title>YugabyteDB</title>"
"    <link rel='shortcut icon' href='/favicon.ico'>"
"    <link href='/bootstrap/css/bootstrap.min.css' rel='stylesheet' media='screen' />"
"    <link href='/bootstrap/css/bootstrap-theme.min.css' rel='stylesheet' media='screen' />"
"    <link href='/font-awesome/css/font-awesome.min.css' rel='stylesheet' media='screen' />"
"    <link href='/yb.css' rel='stylesheet' media='screen' />"
"    <script src='/libs/jquery/3.7.0/jquery-3.7.0.min.js'></script>"
"    <script type='text/javascript' src='/collapse.js'></script>"
"  </head>"
"\n"
"<body>"
"\n";

static const char* const NAVIGATION_BAR_PREFIX =
"  <nav class=\"navbar navbar-fixed-top navbar-inverse sidebar-wrapper\" role=\"navigation\">"
"    <ul class=\"nav sidebar-nav\">"
"      <li><a href='/'><img src='/logo.png' alt='YugabyteDB' class='nav-logo' /></a></li>"
"\n";

static const char* const NAVIGATION_BAR_SUFFIX =
"    </ul>"
"  </nav>"
"\n\n"
"    <div class='yb-main container-fluid'>";

void Webserver::Impl::BootstrapPageHeader(stringstream* output) {
  (*output) << PAGE_HEADER;
  (*output) << NAVIGATION_BAR_PREFIX;
  for (const PathHandlerMap::value_type& handler : path_handlers_) {
    if (handler.second->is_on_nav_bar()) {
      (*output) << "<li class='nav-item'>"
                << "<a href='" << handler.first << "'>"
                << "<div><i class='" << handler.second->icon() << "'aria-hidden='true'></i></div>"
                << handler.second->alias()
                << "</a></li>\n";
    }
  }
  (*output) << NAVIGATION_BAR_SUFFIX;

  if (!static_pages_available()) {
    (*output) << "<div style=\"color: red\"><strong>"
              << "Static pages not available. Configure YB_HOME or use the --webserver_doc_root "
              << "flag to fix page styling.</strong></div>\n";
  }
}

bool Webserver::Impl::static_pages_available() const {
  return !opts_.doc_root.empty() && opts_.enable_doc_root;
}

void Webserver::Impl::set_footer_html(const std::string& html) {
  std::lock_guard l(lock_);
  footer_html_ = html;
}

void Webserver::Impl::BootstrapPageFooter(stringstream* output) {
  SharedLock<std::shared_timed_mutex> l(lock_);
  *output << "<div class='yb-bottom-spacer'></div></div>\n"; // end bootstrap 'container' div
  if (!footer_html_.empty()) {
    *output << "<footer class='footer'><div class='yb-footer container text-muted'>";
    *output << footer_html_;
    *output << "</div></footer>";
  }
  *output << "</body></html>\n";
}

Webserver::Webserver(const WebserverOptions& opts, const std::string& server_name)
    : impl_(std::make_unique<Impl>(opts, server_name)) {
}

Webserver::~Webserver() { }

Status Webserver::Start() {
  return impl_->Start();
}

void Webserver::Stop() {
  impl_->Stop();
}

Status Webserver::GetBoundAddresses(std::vector<Endpoint>* addrs) const {
  return impl_->GetBoundAddresses(addrs);
}

Status Webserver::GetInputHostPort(HostPort* hp) const {
  return impl_->GetInputHostPort(hp);
}

void Webserver::SetLogging(bool enable_access_logging, bool enable_tcmalloc_logging) {
  impl_->SetLogging(enable_access_logging, enable_tcmalloc_logging);
}

void Webserver::RegisterPathHandler(const std::string& path,
                                    const std::string& alias,
                                    const PathHandlerCallback& callback,
                                    bool is_styled,
                                    bool is_on_nav_bar,
                                    const std::string icon) {
  return impl_->RegisterPathHandler(path, alias, callback, is_styled, is_on_nav_bar, icon);
}

void Webserver::set_footer_html(const std::string& html) {
  return impl_->set_footer_html(html);
}

bool Webserver::IsSecure() const {
  return impl_->IsSecure();
}

void Webserver::SetAutoFlags(std::unordered_set<std::string>&& flags) {
  impl_->SetAutoFlags(std::move(flags));
}

bool Webserver::ContainsAutoFlag(const std::string& flag) const {
  return impl_->ContainsAutoFlag(flag);
}

void Webserver::SetFlags(std::unordered_set<std::string>&& flags) {
  impl_->SetFlags(std::move(flags));
}

bool Webserver::ContainsFlag(const std::string& flag) const { return impl_->ContainsFlag(flag); }

const std::string& Webserver::DocRoot() const {
  return impl_->DocRoot();
}
} // namespace yb
