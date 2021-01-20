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

#ifndef YB_YQL_PGGATE_PG_MEMCTX_H_
#define YB_YQL_PGGATE_PG_MEMCTX_H_

#include <vector>
#include <unordered_map>

#include <boost/intrusive/list.hpp>

#include "yb/yql/pggate/pg_tabledesc.h"

namespace yb {
namespace pggate {

// This is the YB counterpart of Postgres's MemoryContext.
// YugaByte memory context hold one reference count to PgGate objects such as PgGate::PgStatement.
// When Postgres process complete execution, it would release the reference count by destroying
// the YugaByte memory context.
//
// - Each YB Memctx will be associated with a Postgres MemoryContext.
// - YB Memctx will be initialized to NULL and later created on its first use.
// - When Postgres MemoryContext is destroyed, YB Memctx will be destroyed.
// - When Postgres MemoryContext allocates YugaByte object, that YB object will belong to the
//   associated YB Memctx. The object is automatically destroyed when YB Memctx is destroyed.
class PgMemctx {
 public:
  typedef std::shared_ptr<PgMemctx> SharedPtr;

  class Registrable : public boost::intrusive::list_base_hook<> {
   public:
    virtual ~Registrable() = default;
   private:
    PgMemctx* memctx_ = nullptr;
    friend class PgMemctx;
  };

  // Constructor and destructor.
  PgMemctx();
  virtual ~PgMemctx();

  // API: Create(), Destroy(), and Reset()
  // - Because Postgres process own YugaByte memory context, only Postgres processes should call
  //   these functions to manage YugaByte memory context.
  // - When Postgres process (a C Program) is exiting, it assumes that all associated memories
  //   are destroyed and will not call Destroy() to free YugaByte memory context. As a result,
  //   PgGate must release the remain YugaByte memory contexts itself. Create(), Destroy(), and
  //   Reset() API uses a global variable for that purpose.  When Postgres processes exit, the
  //   global destructor will free all YugaByte memory contexts.

  // Create yugabyte memory context that will be owned by Postgres process.
  static PgMemctx *Create();

  // Destroy yugabyte memory context that is owned by Postgres process.
  static CHECKED_STATUS Destroy(PgMemctx *handle);

  // Clear the content of yugabyte memory context that is owned by Postgres process.
  // Postgres has Reset() option where it clears the allocated memory for the current context but
  // keeps all the allocated memory for the child contexts.
  static CHECKED_STATUS Reset(PgMemctx *handle);

  void Register(Registrable *obj);
  static void Destroy(Registrable *obj);

  // Cache the table descriptor in the memory context to be destroyed later on.
  void Cache(size_t hash_id, const PgTableDesc::ScopedRefPtr &table_desc);

  // Read the table descriptor from cache.
  void GetCache(size_t hash_id, PgTableDesc **handle);

 private:
  // NOTE:
  // - In Postgres, the objects in the outer context can references to the objects of the nested
  //   context but not vice versa, so it is safe to clear objects of outer context.
  // - In YugaByte, the above abstraction must be followed, but I am not yet sure that we did.
  //   For now we destroy the yugabyte objects in the current context also as we should. However,
  //   if the objects in nested context might still have referenced to the objects of the outer
  //   memctx, we can delay the PgStatement objects' destruction.
  void Clear();

  // All talbe descriptors that are allocated with this memory context.
  std::unordered_map<size_t, PgTableDesc::ScopedRefPtr> tabledesc_map_;

  boost::intrusive::list<Registrable> registered_objects_;

  DISALLOW_COPY_AND_ASSIGN(PgMemctx);
};

}  // namespace pggate
}  // namespace yb

#endif // YB_YQL_PGGATE_PG_MEMCTX_H_
