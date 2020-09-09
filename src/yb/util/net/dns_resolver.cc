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

#include "yb/util/net/dns_resolver.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <boost/optional.hpp>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include "yb/util/concurrent_value.h"
#include "yb/util/flag_tags.h"
#include "yb/util/metrics.h"
#include "yb/util/net/net_util.h"
#include "yb/util/net/sockaddr.h"

using namespace std::literals;

DEFINE_int64(dns_cache_expiration_ms, 60000, "Time to store DNS resolution results in cache.");

namespace yb {

namespace {


Result<IpAddress> PickResolvedAddress(
    const std::string& host, const boost::system::error_code& error,
    const ResolverResults& entries) {
  if (error) {
    return STATUS_FORMAT(NetworkError, "Resolve failed $0: $1", host, error.message());
  }
  std::vector<IpAddress> addresses;
  for (const auto& entry : entries) {
    addresses.push_back(entry.endpoint().address());
    VLOG(3) << "Resolved address " << entry.endpoint().address().to_string()
            << " for host " << host;
  }
  FilterAddresses(FLAGS_net_address_filter, &addresses);
  if (addresses.empty()) {
    return STATUS_FORMAT(NetworkError, "No endpoints resolved for: $0", host);
  }
  if (addresses.size() > 1) {
    LOG(WARNING) << "Peer address '" << host << "' "
                 << "resolves to " << yb::ToString(addresses) << " different addresses. Using "
                 << yb::ToString(addresses.front());
  }

  VLOG(3) << "Returned address " << addresses[0].to_string() << " for host "
          << host;
  return addresses.front();
}

} // namespace

class DnsResolver::Impl {
 public:
  explicit Impl(IoService* io_service) : resolver_(*io_service) {}

  std::shared_future<Result<IpAddress>> ResolveFuture(const std::string& host) {
    return ObtainEntry(host)->DoResolve(host, /* callback= */ nullptr, &resolver_);
  }

  void AsyncResolve(const std::string& host, const AsyncResolveCallback& callback) {
    ObtainEntry(host)->DoResolve(host, &callback, &resolver_);
  }

 private:
  using Resolver = boost::asio::ip::basic_resolver<boost::asio::ip::tcp>;

  struct CacheEntry {
    std::mutex mutex;
    CoarseTimePoint expiration GUARDED_BY(mutex) = CoarseTimePoint::min();
    std::shared_future<Result<IpAddress>> future GUARDED_BY(mutex);
    std::vector<AsyncResolveCallback> waiters GUARDED_BY(mutex);

    void SetResult(
        const Result<IpAddress>& result,
        std::promise<Result<IpAddress>>* promise) EXCLUDES(mutex) {
      try {
        promise->set_value(result);
      } catch (std::future_error& error) {
        return;
      }

      decltype(waiters) to_notify;
      {
        std::lock_guard<std::mutex> lock(mutex);
        expiration = CoarseMonoClock::now() + FLAGS_dns_cache_expiration_ms * 1ms;
        waiters.swap(to_notify);
      }
      for (const auto& waiter : to_notify) {
        waiter(result);
      }
    }

    std::shared_future<Result<IpAddress>> DoResolve(
        const std::string& host, const AsyncResolveCallback* callback, Resolver* resolver) {
      std::shared_ptr<std::promise<Result<IpAddress>>> promise;
      std::shared_future<Result<IpAddress>> result;
      {
        std::lock_guard<std::mutex> lock(mutex);
        promise = StartResolve(host);
        result = future;
        if (callback && expiration == CoarseTimePoint::max()) {
          // Resolve is in progress by a different caller.
          waiters.push_back(*callback);
          callback = nullptr;
        }
      }

      if (callback) {
        (*callback)(result.get());
      }

      if (promise) {
        static const std::string kService = "";
        resolver->async_resolve(
            Resolver::query(host, kService),
            [this, host, promise](
                const boost::system::error_code& error,
                const Resolver::results_type& entries) mutable {
          // Unfortunately there is no safe way to set promise value from 2 different threads, w/o
          // catching exception in case of concurrency.
          SetResult(PickResolvedAddress(host, error, entries), promise.get());
        });

        if (resolver->get_io_context().stopped()) {
          SetResult(STATUS(Aborted, "Messenger already stopped"), promise.get());
        }
      }

      return result;
    }

    std::shared_ptr<std::promise<Result<IpAddress>>> StartResolve(
        const std::string& host) REQUIRES(mutex) {
      if (expiration >= CoarseMonoClock::now()) {
        return nullptr;
      }

      auto promise = std::make_shared<std::promise<Result<IpAddress>>>();
      future = promise->get_future().share();

      auto address = TryFastResolve(host);
      if (address) {
        expiration = CoarseTimePoint::max() - 1ms;
        promise->set_value(*address);
        return nullptr;
      } else {
        expiration = CoarseTimePoint::max();
      }

      return promise;
    }
  };

  CacheEntry* ObtainEntry(const std::string& host) {
    {
      std::shared_lock<decltype(mutex_)> lock(mutex_);
      auto it = cache_.find(host);
      if (it != cache_.end()) {
        return &it->second;
      }
    }

    std::lock_guard<decltype(mutex_)> lock(mutex_);
    return &cache_[host];
  }

  Resolver resolver_;
  std::shared_timed_mutex mutex_;
  std::unordered_map<std::string, CacheEntry> cache_;
};

DnsResolver::DnsResolver(IoService* io_service) : impl_(new Impl(io_service)) {
}

DnsResolver::~DnsResolver() {
}

namespace {

thread_local Histogram* active_metric_ = nullptr;

} // anonymous namespace

ScopedDnsTracker::ScopedDnsTracker(const scoped_refptr<Histogram>& metric)
    : old_metric_(active_metric()), metric_(metric) {
  active_metric_ = metric.get();
}

ScopedDnsTracker::~ScopedDnsTracker() {
  DCHECK_EQ(metric_.get(), active_metric());
  active_metric_ = old_metric_;
}

Histogram* ScopedDnsTracker::active_metric() {
  return active_metric_;
}

std::shared_future<Result<IpAddress>> DnsResolver::ResolveFuture(const std::string& host) {
  return impl_->ResolveFuture(host);
}

void DnsResolver::AsyncResolve(const std::string& host, const AsyncResolveCallback& callback) {
  impl_->AsyncResolve(host, callback);
}

Result<IpAddress> DnsResolver::Resolve(const std::string& host) {
  return ResolveFuture(host).get();
}

} // namespace yb
