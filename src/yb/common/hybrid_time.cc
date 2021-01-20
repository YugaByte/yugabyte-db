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

#include "yb/common/hybrid_time.h"

#include <boost/date_time/c_local_time_adjustor.hpp>
#include <boost/date_time/posix_time/ptime.hpp>
#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/date_time/posix_time/time_formatters.hpp>

#include "yb/util/memcmpable_varint.h"

using std::string;
using strings::Substitute;
using strings::SubstituteAndAppend;

namespace yb {

namespace {

std::atomic<bool> pretty_to_string_mode_{false};

}

const HybridTime HybridTime::kMin(kMinHybridTimeValue);
const HybridTime HybridTime::kMax(kMaxHybridTimeValue);
const HybridTime HybridTime::kInitial(kInitialHybridTimeValue);
const HybridTime HybridTime::kInvalid(kInvalidHybridTimeValue);

bool HybridTime::DecodeFrom(Slice *input) {
  return GetMemcmpableVarint64(input, &v).ok();
}

void HybridTime::AppendAsUint64To(faststring *dst) const {
  PutMemcmpableVarint64(dst, v);
}

void HybridTime::AppendAsUint64To(std::string* dst) const {
  PutMemcmpableVarint64(dst, v);
}

string HybridTime::ToString() const {
  switch (v) {
    case kInvalidHybridTimeValue:
      return "<invalid>";
    case kMaxHybridTimeValue:
      return "<max>";
    case kMinHybridTimeValue:
      return "<min>";
    case kInitialHybridTimeValue:
      return "<initial>";
    default:
      auto logical = GetLogicalValue();
      if (!pretty_to_string_mode_.load(std::memory_order_acquire)) {
        if (logical) {
          return Format("{ physical: $0 logical: $1 }", GetPhysicalValueMicros(), logical);
        } else {
          return Format("{ physical: $0 }", GetPhysicalValueMicros());
        }
      }
      // When time is rendered with separate minutes and seconds it is easier to understand that
      // one value is 2 seconds later that another one.
      // With default rendering difference appears in the middle of 10+ digits number.
      boost::posix_time::ptime start(boost::gregorian::date(1970, 1, 1));
      auto usec = GetPhysicalValueMicros();
      auto utc_time = start + boost::posix_time::microseconds(usec);
      auto local_time =
          boost::date_time::c_local_adjustor<boost::posix_time::ptime>::utc_to_local(utc_time);
      auto date = local_time.date();
      auto time_of_day = local_time.time_of_day();
      auto days = (boost::posix_time::ptime(date) - start).hours() / 24;
      auto time_of_day_str = boost::posix_time::to_simple_string(time_of_day);
      if (logical) {
        return Format("{ days: $0 time: $1 logical: $2 }", days, time_of_day_str, logical);
      } else {
        return Format("{ days: $0 time: $1 }", days, time_of_day_str);
      }
  }
}

void HybridTime::TEST_SetPrettyToString(bool flag) {
  pretty_to_string_mode_ = flag;
}

string HybridTime::ToDebugString() const {
  return kHybridTimeDebugStrPrefix + ToString();
}

uint64_t HybridTime::ToUint64() const {
  return v;
}

Status HybridTime::FromUint64(uint64_t value) {
  v = value;
  return Status::OK();
}

MicrosTime HybridTime::CeilPhysicalValueMicros() const {
  if (*this == kMin) {
    return 0;
  }
  auto result = GetPhysicalValueMicros();
  if (GetLogicalValue()) {
    ++result;
  }
  return result;
}

const char* const HybridTime::kHybridTimeDebugStrPrefix = "HT";

}  // namespace yb
