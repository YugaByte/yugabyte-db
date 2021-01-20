//--------------------------------------------------------------------------------------------------
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
//
// This class represents a QL session of a client connection (e.g. CQL client connection).
//--------------------------------------------------------------------------------------------------
#ifndef YB_YQL_CQL_QL_QL_SESSION_H_
#define YB_YQL_CQL_QL_QL_SESSION_H_

#include <memory>
#include <mutex>
#include <string>

#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include <gflags/gflags.h>

#include "yb/master/master_defaults.h"
#include "yb/util/shared_lock.h"

DECLARE_bool(use_cassandra_authentication);

namespace yb {
namespace ql {

static const char* const kUndefinedKeyspace = ""; // Must be empty string.
static const char* const kUndefinedRoleName = ""; // Must be empty string.

class QLSession {
 public:
  // Public types.
  typedef std::shared_ptr<QLSession> SharedPtr;
  typedef std::shared_ptr<const QLSession> SharedPtrConst;

  // Constructors.
  QLSession() : user_authenticated_(!FLAGS_use_cassandra_authentication) {}

  virtual ~QLSession() {}

  // Access functions for current keyspace. It can be accessed by multiple calls in parallel so
  // they need to be thread-safe for shared reads / exclusive writes.
  std::string current_keyspace() const {
    SharedLock<boost::shared_mutex> l(current_keyspace_mutex_);
    return current_keyspace_;
  }

  void set_current_keyspace(const std::string& keyspace) {
    boost::lock_guard<boost::shared_mutex> l(current_keyspace_mutex_);
    current_keyspace_ = keyspace;
  }

  // Access functions for current role_name. It can be accessed by multiple calls in parallel so
  // they need to be thread-safe for shared reads / exclusive writes.
  std::string current_role_name() const {
    SharedLock<boost::shared_mutex> l(current_role_name_mutex_);
    return current_role_name_;
  }

  void set_current_role_name(const std::string& role_name) {
    boost::lock_guard<boost::shared_mutex> l(current_role_name_mutex_);
    current_role_name_ = role_name;
  }

  // Access functions for 'user is authenticated' flag. It can be accessed by multiple calls in
  // parallel so they need to be thread-safe for shared reads / exclusive writes.
  bool is_user_authenticated() const {
    SharedLock<boost::shared_mutex> l(user_authenticated_mutex_);
    return user_authenticated_;
  }

  void set_user_authenticated(bool authenticated = true) {
    boost::lock_guard<boost::shared_mutex> l(user_authenticated_mutex_);
    user_authenticated_ = authenticated;
  }

 private:
  // Mutexes to protect access to the class fields.
  mutable boost::shared_mutex current_keyspace_mutex_;
  mutable boost::shared_mutex current_role_name_mutex_;
  mutable boost::shared_mutex user_authenticated_mutex_;
  // Current keyspace.
  std::string current_keyspace_ = kUndefinedKeyspace;
  // TODO (Bristy) : After Login has been done, test this.
  std::string current_role_name_ = kUndefinedRoleName;

  bool user_authenticated_;
};

}  // namespace ql
}  // namespace yb

#endif  // YB_YQL_CQL_QL_QL_SESSION_H_
