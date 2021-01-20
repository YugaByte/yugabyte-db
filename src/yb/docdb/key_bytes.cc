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

#include "yb/docdb/key_bytes.h"

#include "yb/common/doc_hybrid_time.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/util/fast_varint.h"

namespace yb {
namespace docdb {

void AppendDocHybridTime(const DocHybridTime& doc_ht, KeyBytes* key) {
  key->AppendValueType(ValueType::kHybridTime);
  doc_ht.AppendEncodedInDocDbFormat(key->mutable_data());
}

void KeyBytes::AppendUInt64AsVarInt(uint64_t value) {
  unsigned char buf[util::kMaxVarIntBufferSize];
  size_t len = 0;
  util::FastEncodeUnsignedVarInt(value, buf, &len);
  AppendRawBytes(Slice(buf, len));
}

}  // namespace docdb
}  // namespace yb
