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

#ifndef YB_MASTER_YQL_VTABLE_ITERATOR_H
#define YB_MASTER_YQL_VTABLE_ITERATOR_H

#include "yb/common/ql_rowwise_iterator_interface.h"
#include "yb/common/ql_scanspec.h"
#include "yb/docdb/doc_key.h"

namespace yb {
namespace master {

// An iterator over a YQLVirtualTable.
class YQLVTableIterator : public common::YQLRowwiseIteratorIf {
 public:
  explicit YQLVTableIterator(const std::unique_ptr<QLRowBlock> vtable);

  void SkipRow() override;

  bool HasNext() const override;

  std::string ToString() const override;

  const Schema &schema() const override;

  HybridTime RestartReadHt() override { return HybridTime::kInvalid; }

  virtual ~YQLVTableIterator();

 private:
  CHECKED_STATUS DoNextRow(const Schema& projection, QLTableRow* table_row) override;

  std::unique_ptr<QLRowBlock> vtable_;
  size_t vtable_index_;
};

}  // namespace master
}  // namespace yb
#endif // YB_MASTER_YQL_VTABLE_ITERATOR_H
