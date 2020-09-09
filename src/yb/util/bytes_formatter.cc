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

#include "yb/util/bytes_formatter.h"

#include "yb/gutil/stringprintf.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/util/cast.h"
#include "yb/util/enums.h"
#include "yb/util/format.h"

using std::string;
using strings::Substitute;

namespace yb {

string FormatBytesAsStr(const char* data,
                        const size_t n,
                        const QuotesType quotes_type,
                        const size_t max_length) {
  string result;

  char quote = '"';
  switch (quotes_type) {
    case QuotesType::kSingleQuotes:
      quote = '\'';
      break;
    case QuotesType::kDoubleQuotes:
      quote = '"';
      break;
  }

  result.push_back(quote);
  const char* end = data + n;

  // Not including the current character we're looking at. Cast to a signed int to avoid underflow.
  int64_t bytes_left = static_cast<int64_t>(n - 1);

  for (const char* p = data; p != end; ++p, --bytes_left) {
    uint8_t c = static_cast<uint8_t>(*p);
    if (c == quote) {
      result.push_back('\\');
      result.push_back(quote);
    } else if (c == '\\') {
      result.append("\\\\");
    } else if (isgraph(c) || c == ' ') {
      result.push_back(c);
    } else {
      result.append(StringPrintf("\\x%02x", c));
    }
    // See if we went above the max size. Don't bother if there is only one byte left to print,
    // so that we can always say "bytes".
    if (result.size() >= max_length && bytes_left > 1) {
      result.append(Substitute("<...$0 bytes skipped>", bytes_left));
      break;
    }
  }
  result.push_back(quote);
  return result;
}

string FormatBytesAsStr(const string& s, QuotesType quotes_type, size_t max_length) {
  return FormatBytesAsStr(s.c_str(), s.size(), quotes_type, max_length);
}

string FormatSliceAsStr(const Slice& s, QuotesType quotes_type, size_t max_length) {
  return FormatBytesAsStr(util::to_char_ptr(s.data()), s.size(), quotes_type, max_length);
}

std::string FormatSliceAsStr(
    const yb::Slice& slice,
    BinaryOutputFormat output_format,
    QuotesType quote_type,
    size_t max_length) {
  switch (output_format) {
    case BinaryOutputFormat::kEscaped:
      return FormatSliceAsStr(slice, quote_type, max_length);
    case BinaryOutputFormat::kHex:
      return slice.ToDebugHexString();
    case BinaryOutputFormat::kEscapedAndHex:
      return Format(
          "$0 ($1)",
          FormatSliceAsStr(slice, quote_type, max_length), slice.ToDebugHexString());
  }
  FATAL_INVALID_ENUM_VALUE(BinaryOutputFormat, output_format);
}

}  // namespace yb
