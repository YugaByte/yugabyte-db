// Copyright (c) YugaByte, Inc.

#include "yb/docdb/in_mem_docdb.h"

#include <sstream>

#include "rocksdb/db.h"

#include "yb/common/hybrid_time.h"
#include "yb/docdb/docdb.h"
#include "yb/docdb/docdb_rocksdb_util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/rocksutil/yb_rocksdb.h"

using std::endl;
using std::string;
using std::stringstream;
using strings::Substitute;

namespace yb {
namespace docdb {

Status InMemDocDbState::SetPrimitive(const DocPath& doc_path, const PrimitiveValue& value) {
  VLOG(2) << __func__ << ": doc_path=" << doc_path.ToString() << ", value=" << value.ToString();
  const PrimitiveValue encoded_doc_key_as_primitive(doc_path.encoded_doc_key().AsStringRef());
  const bool is_deletion = value.value_type() == ValueType::kTombstone;
  if (doc_path.num_subkeys() == 0) {
    if (is_deletion) {
      root_.DeleteChild(encoded_doc_key_as_primitive);
    } else {
      root_.SetChildPrimitive(encoded_doc_key_as_primitive, value);
    }

    return Status::OK();
  }
  SubDocument* current_subdoc = nullptr;

  if (is_deletion) {
    current_subdoc = root_.GetChild(encoded_doc_key_as_primitive);
    if (current_subdoc == nullptr) {
      // The subdocument we're trying to delete does not exist, nothing to do.
      return Status::OK();
    }
  } else {
    current_subdoc = root_.GetOrAddChild(encoded_doc_key_as_primitive).first;
  }

  const int num_subkeys = doc_path.num_subkeys();
  for (int subkey_index = 0; subkey_index < num_subkeys - 1; ++subkey_index) {
    const PrimitiveValue& subkey = doc_path.subkey(subkey_index);
    if (subkey.value_type() == ValueType::kArrayIndex) {
      return STATUS(NotSupported, "Setting values at a given array index is not supported yet.");
    }

    if (current_subdoc->value_type() != ValueType::kObject) {
      return STATUS(IllegalState, Substitute(
          "Cannot set or delete values inside a subdocument of type $0",
          ValueTypeToStr(current_subdoc->value_type())));
    }

    if (is_deletion) {
      current_subdoc = current_subdoc->GetChild(subkey);
      if (current_subdoc == nullptr) {
        // Document does not exist, nothing to do.
        return Status::OK();
      }
    } else {
      current_subdoc = current_subdoc->GetOrAddChild(subkey).first;
    }
  }

  if (is_deletion) {
    current_subdoc->DeleteChild(doc_path.last_subkey());
  } else {
    current_subdoc->SetChildPrimitive(doc_path.last_subkey(), value);
  }

  return Status::OK();
}

Status InMemDocDbState::DeleteSubDoc(const DocPath &doc_path) {
  return SetPrimitive(doc_path, PrimitiveValue(ValueType::kTombstone));
}

void InMemDocDbState::SetDocument(const KeyBytes& encoded_doc_key, SubDocument&& doc) {
  root_.SetChild(PrimitiveValue(encoded_doc_key.AsStringRef()), std::move(doc));
}

const SubDocument* InMemDocDbState::GetSubDocument(const SubDocKey& subdoc_key) const {
  const SubDocument* current =
      root_.GetChild(PrimitiveValue(subdoc_key.doc_key().Encode().AsStringRef()));
  for (const auto& subkey : subdoc_key.subkeys()) {
    if (current == nullptr) {
      return nullptr;
    }
    current = current->GetChild(subkey);
  }
  return current;
}

void InMemDocDbState::CaptureAt(rocksdb::DB* rocksdb, HybridTime hybrid_time,
                                rocksdb::QueryId query_id) {
  // Clear the internal state.
  root_ = SubDocument();

  auto rocksdb_iter = CreateRocksDBIterator(rocksdb, BloomFilterMode::DONT_USE_BLOOM_FILTER,
                                            query_id);
  rocksdb_iter->SeekToFirst();
  KeyBytes prev_key;
  while (rocksdb_iter->Valid()) {
    const auto key = rocksdb_iter->key();
    CHECK_NE(0, prev_key.CompareTo(key)) << "Infinite loop detected on key " << prev_key.ToString();
    prev_key = KeyBytes(key);

    SubDocKey subdoc_key;
    CHECK_OK(subdoc_key.FullyDecodeFrom(key));
    CHECK_EQ(0, subdoc_key.num_subkeys())
        << "Expected to be positioned at the first key of a new document with no subkeys, "
        << "but found " << subdoc_key.num_subkeys() << " subkeys: " << subdoc_key.ToString();
    subdoc_key.remove_hybrid_time();

    bool doc_found = false;
    SubDocument subdoc;
    // TODO: It would be good to be able to refer to a slice of the original key whenever we need
    //       to extract document key out of a subdocument key.
    auto encoded_doc_key = subdoc_key.doc_key().Encode();
    const Status get_doc_status =
        yb::docdb::GetSubDocument(rocksdb, subdoc_key, &subdoc, &doc_found, query_id, hybrid_time);
    if (!get_doc_status.ok()) {
      // This will help with debugging the GetSubDocument failure.
      LOG(WARNING) << "DocDB state:\n" << DocDBDebugDumpToStr(rocksdb, /* include_binary = */ true);
    }
    CHECK_OK(get_doc_status);
    // doc_found can be false for deleted documents, and that is perfectly valid.
    if (doc_found) {
      SetDocument(encoded_doc_key, std::move(subdoc));
    }
    // Go to the next top-level document key.
    ROCKSDB_SEEK(rocksdb_iter.get(), subdoc_key.AdvanceOutOfSubDoc().AsSlice());

    VLOG(4) << "After performing a seek: IsValid=" << rocksdb_iter->Valid();
    if (VLOG_IS_ON(4) && rocksdb_iter->Valid()) {
      VLOG(4) << "Next key: " << FormatRocksDBSliceAsStr(rocksdb_iter->key());
      SubDocKey tmp_subdoc_key;
      CHECK_OK(tmp_subdoc_key.FullyDecodeFrom(rocksdb_iter->key()));
      VLOG(4) << "Parsed as SubDocKey: " << tmp_subdoc_key.ToString();
    }
  }

  // Initialize the "captured at" hybrid_time now, even though we expect it to be overwritten in
  // many cases. One common usage pattern is that this will be called with HybridTime::kMax, but
  // we'll later call SetCaptureHybridTime and set the hybrid_time to the last known hybrid_time of
  // an operation performed on DocDB.
  captured_at_ = hybrid_time;

  // Ensure we don't get any funny value types in the root node (had a test failure like this).
  CHECK_EQ(root_.value_type(), ValueType::kObject);
}

void InMemDocDbState::SetCaptureHybridTime(HybridTime hybrid_time) {
  CHECK_NE(hybrid_time, HybridTime::kInvalidHybridTime);
  captured_at_ = hybrid_time;
}

bool InMemDocDbState::EqualsAndLogDiff(const InMemDocDbState &expected, bool log_diff) {
  bool matches = true;
  if (num_docs() != expected.num_docs()) {
    if (log_diff) {
      LOG(WARNING) << "Found " << num_docs() << " documents but expected to find "
                   << expected.num_docs();
    }
    matches = false;
  }

  // As an optimization, a SubDocument won't even maintain a map if it is an empty object that no
  // operations have been performed on. As we are using a SubDocument to represent the top-level
  // mapping of encoded document keys to SubDocuments here, we need to check for that situation.
  if (expected.root_.has_valid_object_container()) {
    for (const auto& expected_kv : expected.root_.object_container()) {
      const KeyBytes encoded_doc_key(expected_kv.first.GetString());
      const SubDocument& expected_doc = expected_kv.second;
      DocKey doc_key;
      CHECK_OK(doc_key.FullyDecodeFrom(encoded_doc_key.AsSlice()));
      const SubDocument* child_from_this = GetDocument(doc_key);
      if (child_from_this == nullptr) {
        if (log_diff) {
          LOG(WARNING) << "Document with key " << doc_key.ToString() << " is missing but is "
                       << "expected to be " << expected_doc.ToString();
        }
        matches = false;
        continue;
      }
      if (*child_from_this != expected_kv.second) {
        if (log_diff) {
          LOG(WARNING) << "Expected document with key " << doc_key.ToString() << " to be "
                       << expected_doc.ToString() << " but found " << *child_from_this;
        }
        matches = false;
      }
    }
  }

  // Also report all document keys that are present in this ("actual") database but are absent from
  // the other ("expected") database.
  if (root_.has_valid_object_container()) {
    for (const auto& actual_kv : root_.object_container()) {
      const KeyBytes encoded_doc_key(actual_kv.first.GetString());
      DocKey doc_key;
      CHECK_OK(doc_key.FullyDecodeFrom(encoded_doc_key.AsSlice()));
      const SubDocument* child_from_expected = GetDocument(doc_key);
      if (child_from_expected == nullptr) {
        DocKey doc_key;
        CHECK_OK(doc_key.FullyDecodeFrom(encoded_doc_key.AsSlice()));
        if (log_diff) {
          LOG(WARNING) << "Unexpected document found with key " << doc_key.ToString() << ":"
                       << actual_kv.second.ToString();
        }
        matches = false;
      }
    }
  }

  // A brute-force way to check that the comparison logic above is correct.
  // TODO: disable this if it makes tests much slower.
  CHECK_EQ(matches, ToDebugString() == expected.ToDebugString());
  return matches;
}

string InMemDocDbState::ToDebugString() const {
  stringstream ss;
  if (root_.has_valid_object_container()) {
    int i = 1;
    for (const auto& kv : root_.object_container()) {
      DocKey doc_key;
      CHECK_OK(doc_key.FullyDecodeFrom(rocksdb::Slice(kv.first.GetString())));
      ss << i << ". " << doc_key.ToString() << " => " << kv.second.ToString() << endl;
      ++i;
    }
  }
  string dump_str = ss.str();
  return dump_str.empty() ? "<Empty>" : dump_str;
}

HybridTime InMemDocDbState::captured_at() const {
  CHECK_NE(captured_at_, HybridTime::kInvalidHybridTime);
  return captured_at_;
}

void InMemDocDbState::SanityCheck() const {
  CHECK_EQ(root_.value_type(), ValueType::kObject);
}

}  // namespace docdb
}  // namespace yb
