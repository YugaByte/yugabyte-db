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

#include "yb/util/curl_util.h"

#include <curl/curl.h>

#include <vector>

#include "yb/util/format.h"
#include "yb/util/logging.h"

#include "yb/gutil/casts.h"

#include "yb/util/faststring.h"
#include "yb/util/scope_exit.h"
#include "yb/util/status.h"

using std::string;

namespace yb {

namespace {

inline Status TranslateError(CURLcode code) {
  if (code == CURLE_OK) {
    return Status::OK();
  }
  return STATUS(NetworkError, "curl error", curl_easy_strerror(code));
}

extern "C" {
size_t WriteCallback(void* buffer, size_t size, size_t nmemb, void* user_ptr) {
  size_t real_size = size * nmemb;
  faststring* buf = reinterpret_cast<faststring*>(user_ptr);
  CHECK_NOTNULL(buf)->append(reinterpret_cast<const uint8_t*>(buffer), real_size);
  return real_size;
}
} // extern "C"

} // anonymous namespace

CurlGlobalInitializer::CurlGlobalInitializer() {
  CHECK_EQ(curl_global_init(CURL_GLOBAL_ALL), CURLE_OK);
}

CurlGlobalInitializer::~CurlGlobalInitializer() { curl_global_cleanup(); }

EasyCurl::EasyCurl() {
  curl_ = curl_easy_init();
  CHECK(curl_) << "Could not init curl";
}

EasyCurl::~EasyCurl() {
  curl_easy_cleanup(curl_);
}

Status EasyCurl::FetchURL(
    const string& url, faststring* buf, int64_t timeout_sec,
    const std::vector<std::string>& headers) {
  return DoRequest(url, std::nullopt, std::nullopt, timeout_sec, buf, headers);
}

Status EasyCurl::PostToURL(
    const string& url, const string& post_data, faststring* dst, int64_t timeout_sec) {
  return DoRequest(url, post_data, string("application/x-www-form-urlencoded"), timeout_sec, dst,
                   {} /* headers */);
}

Status EasyCurl::PostToURL(
    const string& url,
    const string& post_data,
    const string& content_type,
    faststring* dst,
    int64_t timeout_sec) {
  return DoRequest(url, post_data, content_type, timeout_sec, dst, {} /* headers */);
}

string EasyCurl::EscapeString(const string& data) {
  string escaped_str;
  auto str = curl_easy_escape(curl_, data.c_str(), narrow_cast<int>(data.length()));
  if (str) {
    escaped_str = str;
    curl_free(str);
  }
  return escaped_str;
}

Status EasyCurl::DoRequest(
    const string& url, const std::optional<const string>& post_data,
    const std::optional<const string>& content_type, int64_t timeout_sec, faststring* dst,
    const std::vector<std::string>& headers) {
  CHECK_NOTNULL(dst)->clear();

  // Add headers if specified.
  struct curl_slist* curl_headers = nullptr;
  auto clean_up_curl_slist = ScopeExit([&]() {
    curl_slist_free_all(curl_headers);
  });

  for (const auto& header : headers) {
    curl_headers = CHECK_NOTNULL(curl_slist_append(curl_headers, header.c_str()));
  }
  RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, curl_headers)));

  RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_URL, url.c_str())));
  if (return_headers_) {
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_HEADER, 1)));
  }
  RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback)));
  RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_WRITEDATA,
                                                static_cast<void *>(dst))));
  if (!ca_cert_.empty()) {
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_CAINFO, ca_cert_.c_str())));
  }
  if (follow_redirects_) {
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1)));
  }
  if (ssl_version_ != 0) {
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_SSLVERSION, ssl_version_)));
  }
  if (!cipher_list_.empty()) {
    RETURN_NOT_OK(
        TranslateError(curl_easy_setopt(curl_, CURLOPT_SSL_CIPHER_LIST, cipher_list_.c_str())));
  }
  if (!auth_creds_.empty()) {
    // CURLAUTH_ANY lets curl pick whatever scheme the server advertises in its 401
    // challenge (the embedded webserver uses Digest).
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_HTTPAUTH, CURLAUTH_ANY)));
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_USERPWD, auth_creds_.c_str())));
  }

  typedef std::unique_ptr<curl_slist, std::function<void(curl_slist*)>> CurlSlistPtr;
  CurlSlistPtr http_header_list;
  if (content_type) {
    auto list =
        curl_slist_append(NULL, Format("Content-Type: $0", *content_type).c_str());

    if (!list) {
      return STATUS(InternalError, "Unable to set Content-Type header field");
    }

    http_header_list = CurlSlistPtr(list, [](curl_slist *list) {
      if (list != nullptr) {
        curl_slist_free_all(list);
      }
    });

    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_HTTPHEADER,
                                                  http_header_list.get())));
  }

  if (timeout_sec > 0) {
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout_sec)));
  }

  if (post_data) {
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, post_data->c_str())));
    RETURN_NOT_OK(TranslateError(curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE,
                                                  post_data->size())));
  }

  RETURN_NOT_OK(TranslateError(curl_easy_perform(curl_)));
  long rc; // NOLINT(runtime/int) curl wants a long
  RETURN_NOT_OK(TranslateError(curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &rc)));
  if (rc != 200) {
    return STATUS(RemoteError, Format("HTTP $0", rc));
  }

  return Status::OK();
}

} // namespace yb
