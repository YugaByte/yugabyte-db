// Copyright (c) YugaByte, Inc.

#ifndef YB_CONSENSUS_LEADER_LEASE_H
#define YB_CONSENSUS_LEADER_LEASE_H

#include "yb/util/enums.h"

DECLARE_int32(leader_lease_duration_ms);
DECLARE_int32(ht_lease_duration_ms);

namespace yb {
namespace consensus {

YB_DEFINE_ENUM(LeaderLeaseCheckMode, (NEED_LEASE)(DONT_NEED_LEASE))

template <class Time>
struct LeaseTraits;

template <>
struct LeaseTraits<CoarseTimePoint> {
  static CoarseTimePoint NoneValue() {
    return CoarseTimePoint::min();
  }
};

template <>
struct LeaseTraits<MicrosTime> {
  static MicrosTime NoneValue() {
    return 0;
  }
};

template <class Time>
struct LeaseData {
  typedef LeaseTraits<Time> Traits;

  static Time NoneValue() {
    return Traits::NoneValue();
  }

  LeaseData() : expiration(NoneValue()) {}

  LeaseData(std::string holder_uuid_, const Time& expiration_)
      : holder_uuid(std::move(holder_uuid_)), expiration(expiration_) {}

  // UUID of node that holds leader lease.
  std::string holder_uuid;

  Time expiration;

  void Reset() {
    expiration = NoneValue();
    holder_uuid.clear();
  }

  void TryUpdate(const LeaseData& rhs) {
    if (rhs.expiration > expiration) {
      expiration = rhs.expiration;
      if (rhs.holder_uuid != holder_uuid) {
        holder_uuid = rhs.holder_uuid;
      }
    }
  }

  explicit operator bool() const {
    return expiration != NoneValue();
  }
};

typedef LeaseData<CoarseTimePoint> CoarseTimeLease;

typedef LeaseData<MicrosTime> PhysicalComponentLease;

} // namespace consensus
} // namespace yb

#endif // YB_CONSENSUS_LEADER_LEASE_H
