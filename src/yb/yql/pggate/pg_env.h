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
//--------------------------------------------------------------------------------------------------

// TODO(neil) PgEnv defines the interface for the environment where PostgreSQL engine is running.
// Although postgres libraries might handle most of the environment variables, YugaByte libraries
// might need to deal with some of them. This class is provided for that reason.  This class can
// be removed if YugaByte layers, especially DocDB, do not handle any custom values for ENV.


#ifndef YB_YQL_PGGATE_PG_ENV_H_
#define YB_YQL_PGGATE_PG_ENV_H_

#include <memory>
#include <string>

#include <boost/functional/hash/hash.hpp>

#include "yb/common/entity_ids.h"
#include "yb/client/client.h"

namespace yb {
namespace pggate {

// Postgres object identifier (OID).
typedef uint32_t PgOid;
static constexpr PgOid kPgInvalidOid = 0;
static constexpr PgOid kPgByteArrayOid = 17;

// A struct to identify a Postgres object by oid and the database oid it belongs to.
struct PgObjectId {
  PgOid database_oid = kPgInvalidOid;
  PgOid object_oid = kPgInvalidOid;

  PgObjectId(const PgOid database_oid, const PgOid object_oid)
      : database_oid(database_oid), object_oid(object_oid) {}
  PgObjectId()
      : database_oid(kPgInvalidOid), object_oid(kPgInvalidOid) {}
  explicit PgObjectId(const TableId& table_id) {
    auto res = GetPgsqlDatabaseOidByTableId(table_id);
    if (res.ok()) {
      database_oid = res.get();
    }
    res = GetPgsqlTableOid(table_id);
    if (res.ok()) {
      object_oid = res.get();
    } else {
      // Reset the previously set database_oid.
      database_oid = kPgInvalidOid;
    }
  }

  bool IsValid() const {
    return database_oid != kPgInvalidOid && object_oid != kPgInvalidOid;
  }

  TableId GetYBTableId() const {
    return GetPgsqlTableId(database_oid, object_oid);
  }

  TablegroupId GetYBTablegroupId() const {
    return GetPgsqlTablegroupId(database_oid, object_oid);
  }

  std::string ToString() const {
    return Format("{$0, $1}", database_oid, object_oid);
  }

  bool operator== (const PgObjectId& other) const {
    return database_oid == other.database_oid && object_oid == other.object_oid;
  }

  friend std::size_t hash_value(const PgObjectId& id) {
    std::size_t value = 0;
    boost::hash_combine(value, id.database_oid);
    boost::hash_combine(value, id.object_oid);
    return value;
  }
};

typedef boost::hash<PgObjectId> PgObjectIdHash;

inline std::ostream& operator<<(std::ostream& out, const PgObjectId& id) {
  return out << id.ToString();
}

//------------------------------------------------------------------------------------------------

class PgEnv {
 public:
  // Public types and constants.
  typedef std::unique_ptr<PgEnv> UniPtr;
  typedef std::unique_ptr<const PgEnv> UniPtrConst;

  typedef std::shared_ptr<PgEnv> SharedPtr;
  typedef std::shared_ptr<const PgEnv> SharedPtrConst;

  // Constructor.
  PgEnv() { }
  virtual ~PgEnv() { }
};


}  // namespace pggate
}  // namespace yb

#endif  // YB_YQL_PGGATE_PG_ENV_H_
