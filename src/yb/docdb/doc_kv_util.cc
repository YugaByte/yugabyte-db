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

#include "yb/docdb/doc_kv_util.h"

#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_ttl_util.h"
#include "yb/docdb/value.h"
#include "yb/docdb/docdb-internal.h"
#include "yb/rocksutil/yb_rocksdb.h"
#include "yb/server/hybrid_clock.h"
#include "yb/util/bytes_formatter.h"

using std::string;

using strings::Substitute;
using yb::HybridTime;
using yb::FormatBytesAsStr;

namespace yb {
namespace docdb {

bool KeyBelongsToDocKeyInTest(const rocksdb::Slice &key, const string &encoded_doc_key) {
  if (key.starts_with(encoded_doc_key)) {
    const int encoded_doc_key_size = encoded_doc_key.size();
    const char* key_data = key.cdata();
    return key.size() >= encoded_doc_key_size + 2 &&
           key_data[encoded_doc_key_size] == '\0' &&
           key_data[encoded_doc_key_size + 1] == '\0';
  } else {
    return false;
  }
}

Status ConsumeHybridTimeFromKey(rocksdb::Slice* slice, DocHybridTime* hybrid_time)  {
  return hybrid_time->DecodeFrom(slice);
}

Status DecodeHybridTimeFromEndOfKey(
    const rocksdb::Slice &key,
    DocHybridTime *dest) {
  return dest->DecodeFromEnd(key);
}

// Given a DocDB key stored in RocksDB, validate the DocHybridTime size stored as the
// last few bits of the final byte of the key, and ensure that the ValueType byte preceding that
// encoded DocHybridTime is ValueType::kHybridTime.
Status CheckHybridTimeSizeAndValueType(
    const rocksdb::Slice& key,
    int* ht_byte_size_dest) {
  RETURN_NOT_OK(
      DocHybridTime::CheckAndGetEncodedSize(key, ht_byte_size_dest));
  const size_t hybrid_time_value_type_offset = key.size() - *ht_byte_size_dest - 1;
  const ValueType value_type = DecodeValueType(key[hybrid_time_value_type_offset]);
  if (value_type != ValueType::kHybridTime) {
    return STATUS_FORMAT(
        Corruption,
        "Expected to find value type kHybridTime preceding the HybridTime component of the "
            "encoded key, found $0. DocHybridTime bytes: $1",
        value_type,
        ToShortDebugStr(rocksdb::Slice(key.data() + hybrid_time_value_type_offset,
                                       key.size() - hybrid_time_value_type_offset)));
  }

  return Status::OK();
}

template <char END_OF_STRING>
void AppendEncodedStrToKey(const string &s, KeyBuffer *dest) {
  static_assert(END_OF_STRING == '\0' || END_OF_STRING == '\xff',
                "Only characters '\0' and '\xff' allowed as a template parameter");
  if (END_OF_STRING == '\0' && s.find('\0') == string::npos) {
    // Fast path: no zero characters, nothing to encode.
    dest->append(s);
  } else {
    for (char c : s) {
      if (c == '\0') {
        dest->push_back(END_OF_STRING);
        dest->push_back(END_OF_STRING ^ 1);
      } else {
        dest->push_back(END_OF_STRING ^ c);
      }
    }
  }
}

void AppendZeroEncodedStrToKey(const string &s, KeyBuffer *dest) {
  AppendEncodedStrToKey<'\0'>(s, dest);
}

void AppendComplementZeroEncodedStrToKey(const string &s, KeyBuffer *dest) {
  AppendEncodedStrToKey<'\xff'>(s, dest);
}

template <char A>
inline void TerminateEncodedKeyStr(KeyBuffer *dest) {
  dest->push_back(A);
  dest->push_back(A);
}

void TerminateZeroEncodedKeyStr(KeyBuffer *dest) {
  TerminateEncodedKeyStr<'\0'>(dest);
}

void TerminateComplementZeroEncodedKeyStr(KeyBuffer *dest) {
  TerminateEncodedKeyStr<'\xff'>(dest);
}

template<char END_OF_STRING>
Status DecodeEncodedStr(rocksdb::Slice* slice, string* result) {
  static_assert(END_OF_STRING == '\0' || END_OF_STRING == '\xff',
                "Invalid END_OF_STRING character. Only '\0' and '\xff' accepted");
  constexpr char END_OF_STRING_ESCAPE = END_OF_STRING ^ 1;
  const char* p = slice->cdata();
  const char* end = p + slice->size();

  while (p != end) {
    if (*p == END_OF_STRING) {
      ++p;
      if (p == end) {
        return STATUS(Corruption, StringPrintf("Encoded string ends with only one \\0x%02x ",
                                               END_OF_STRING));
      }
      if (*p == END_OF_STRING) {
        // Found two END_OF_STRING characters, this is the end of the encoded string.
        ++p;
        break;
      }
      if (*p == END_OF_STRING_ESCAPE) {
        // 0 is encoded as 00 01 in ascending encoding and FF FE in descending encoding.
        if (result != nullptr) {
          result->push_back(0);
        }
        ++p;
      } else {
        return STATUS(Corruption, StringPrintf(
            "Invalid sequence in encoded string: "
            R"#(\0x%02x\0x%02x (must be either \0x%02x\0x%02x or \0x%02x\0x%02x))#",
            END_OF_STRING, *p, END_OF_STRING, END_OF_STRING, END_OF_STRING, END_OF_STRING_ESCAPE));
      }
    } else {
      if (result != nullptr) {
        result->push_back((*p) ^ END_OF_STRING);
      }
      ++p;
    }
  }
  if (result != nullptr) {
    result->shrink_to_fit();
  }
  slice->remove_prefix(p - slice->cdata());
  return Status::OK();
}

Status DecodeComplementZeroEncodedStr(rocksdb::Slice* slice, std::string* result) {
  return DecodeEncodedStr<'\xff'>(slice, result);
}

Status DecodeZeroEncodedStr(rocksdb::Slice* slice, string* result) {
  return DecodeEncodedStr<'\0'>(slice, result);
}

string DecodeZeroEncodedStr(string encoded_str) {
  string result;
  rocksdb::Slice slice(encoded_str);
  Status status = DecodeZeroEncodedStr(&slice, &result);
  if (!status.ok()) {
    LOG(FATAL) << "Failed to decode zero-encoded string " << FormatBytesAsStr(encoded_str) << ": "
               << status.ToString();
  }
  if (!slice.empty()) {
    LOG(FATAL) << "Did not consume all characters from a zero-encoded string "
               << FormatBytesAsStr(encoded_str) << ": "
               << "bytes left: " << slice.size() << ", "
               << "encoded_str.size(): " << encoded_str.size();
  }
  return result;
}

std::string ToShortDebugStr(rocksdb::Slice slice) {
  return FormatSliceAsStr(slice, QuotesType::kDoubleQuotes, kShortDebugStringLength);
}

Result<DocHybridTime> DecodeInvertedDocHt(Slice key_slice) {
  if (key_slice.empty() || key_slice.size() > kMaxBytesPerEncodedHybridTime + 1) {
    return STATUS_FORMAT(
        Corruption,
        "Invalid doc hybrid time in reverse intent record suffix: $0",
        key_slice.ToDebugHexString());
  }
  size_t doc_ht_buffer[kMaxWordsPerEncodedHybridTimeWithValueType];
  memcpy(doc_ht_buffer, key_slice.data(), key_slice.size());
  for (size_t i = 0; i != kMaxWordsPerEncodedHybridTimeWithValueType; ++i) {
    doc_ht_buffer[i] = ~doc_ht_buffer[i];
  }
  key_slice = Slice(pointer_cast<char*>(doc_ht_buffer), key_slice.size());

  if (static_cast<ValueType>(key_slice[0]) != ValueType::kHybridTime) {
    return STATUS_FORMAT(
        Corruption,
        "Invalid prefix of doc hybrid time in reverse intent record decoded suffix: $0",
        key_slice.ToDebugHexString());
  }
  key_slice.consume_byte();
  DocHybridTime doc_ht;
  RETURN_NOT_OK(doc_ht.DecodeFrom(&key_slice));
  return doc_ht;
}

}  // namespace docdb
}  // namespace yb
