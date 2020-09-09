//
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

#ifndef YB_UTIL_TOSTRING_H
#define YB_UTIL_TOSTRING_H

#include <chrono>
#include <string>
#include <type_traits>

#include <boost/lexical_cast.hpp>
#include <boost/mpl/and.hpp>

#include <boost/preprocessor/facilities/apply.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

#include "yb/gutil/strings/numbers.h"

#include "yb/util/type_traits.h"

// We should use separate namespace for some checkers.
// Because there could be cases when operator<< is available in yb namespace, but
// unavailable to boost::lexical_cast.
// For instance MonoDelta is implicitly constructible from std::chrono::duration
// and has operator<<.
// So when we are trying to convert std::chrono::duration to string, SupportsOutputToStream
// reports true, but boost::lexical_cast cannot output std::chrono::duration to stream.
// Because such operator<< could not be found using ADL.
namespace yb_tostring {

template <class T>
struct SupportsOutputToStream {
  typedef int Yes;
  typedef struct { Yes array[2]; } No;
  typedef typename std::remove_cv<typename std::remove_reference<T>::type>::type CleanedT;

  template <class U>
  static auto Test(std::ostream* out, const U* u) -> decltype(*out << *u, Yes(0)) {}
  static No Test(...) {}

  static constexpr bool value =
      sizeof(Test(nullptr, static_cast<const CleanedT*>(nullptr))) == sizeof(Yes);
};

HAS_FREE_FUNCTION(to_string);

} // namespace yb_tostring

// This utility actively uses SFINAE (http://en.cppreference.com/w/cpp/language/sfinae)
// technique to route ToString to correct implementation.
namespace yb {

// If class has ToString member function - use it.
HAS_MEMBER_FUNCTION(ToString);
HAS_MEMBER_FUNCTION(to_string);

template <class T>
typename std::enable_if<HasMemberFunction_ToString<T>::value, std::string>::type
ToString(const T& value) {
  return value.ToString();
}

template <class T>
typename std::enable_if<HasMemberFunction_to_string<T>::value, std::string>::type
ToString(const T& value) {
  return value.to_string();
}

// If class has ShortDebugString member function - use it. For protobuf classes mostly.
HAS_MEMBER_FUNCTION(ShortDebugString);

template <class T>
typename std::enable_if<HasMemberFunction_ShortDebugString<T>::value, std::string>::type
ToString(const T& value) {
  return value.ShortDebugString();
}

// Various variants of integer types.
template <class Int>
typename std::enable_if<(sizeof(Int) > 4) && std::is_signed<Int>::value, char*>::type
IntToBuffer(Int value, char* buffer) {
  return FastInt64ToBufferLeft(value, buffer);
}

template <class Int>
typename std::enable_if<(sizeof(Int) > 4) && !std::is_signed<Int>::value, char*>::type
IntToBuffer(Int value, char* buffer) {
  return FastUInt64ToBufferLeft(value, buffer);
}

template <class Int>
typename std::enable_if<(sizeof(Int) <= 4) && std::is_signed<Int>::value, char*>::type
IntToBuffer(Int value, char* buffer) {
  return FastInt32ToBufferLeft(value, buffer);
}

template <class Int>
typename std::enable_if<(sizeof(Int) <= 4) && !std::is_signed<Int>::value, char*>::type
IntToBuffer(Int value, char* buffer) {
  return FastUInt32ToBufferLeft(value, buffer);
}

template <class Int>
typename std::enable_if<std::is_integral<typename std::remove_reference<Int>::type>::value,
                        std::string>::type ToString(Int&& value) {
  char buffer[kFastToBufferSize];
  auto end = IntToBuffer(value, buffer);
  return std::string(buffer, end);
}

template <class Pointer>
class PointerToString {
 public:
  template<class P>
  static std::string Apply(P&& ptr);
};

template <>
class PointerToString<const void*> {
 public:
  static std::string Apply(const void* ptr) {
    if (ptr) {
      char buffer[kFastToBufferSize]; // kFastToBufferSize has enough extra capacity for 0x
      buffer[0] = '0';
      buffer[1] = 'x';
      FastHex64ToBuffer(reinterpret_cast<size_t>(ptr), buffer + 2);
      return buffer;
    } else {
      return "<NULL>";
    }
  }
};

template <>
class PointerToString<void*> {
 public:
  static std::string Apply(const void* ptr) {
    return PointerToString<const void*>::Apply(ptr);
  }
};

template <class Pointer>
typename std::enable_if<IsPointerLike<Pointer>::value, std::string>::type
    ToString(Pointer&& value) {
  typedef typename std::remove_cv<typename std::remove_reference<Pointer>::type>::type CleanedT;
  return PointerToString<CleanedT>::Apply(value);
}

inline const std::string& ToString(const std::string& str) { return str; }
inline std::string ToString(const char* str) { return str; }

template <class First, class Second>
std::string ToString(const std::pair<First, Second>& pair);

template <class Collection>
std::string CollectionToString(const Collection& collection);

template <class T>
typename std::enable_if<yb_tostring::HasFreeFunction_to_string<T>::value,
                        std::string>::type ToString(const T& value) {
  return to_string(value);
}

template <class T>
typename std::enable_if<IsCollection<T>::value &&
                            !yb_tostring::HasFreeFunction_to_string<T>::value &&
                            !HasMemberFunction_ToString<T>::value,
                        std::string>::type ToString(const T& value) {
  return CollectionToString(value);
}

template <class T>
typename std::enable_if<
    boost::mpl::and_<
        boost::mpl::bool_<yb_tostring::SupportsOutputToStream<T>::value>,
        boost::mpl::bool_<!
            (IsPointerLike<T>::value ||
             std::is_integral<typename std::remove_reference<T>::type>::value ||
             IsCollection<T>::value ||
             HasMemberFunction_ToString<T>::value ||
             HasMemberFunction_to_string<T>::value)>
    >::value,
    std::string>::type
ToString(T&& value) {
  return boost::lexical_cast<std::string>(value);
}

// Definition of functions that use ToString chaining should be declared after all declarations.
template <class Pointer>
template <class P>
std::string PointerToString<Pointer>::Apply(P&& ptr) {
  if (ptr) {
    char buffer[kFastToBufferSize]; // kFastToBufferSize has enough extra capacity for 0x and ->
    buffer[0] = '0';
    buffer[1] = 'x';
    FastHex64ToBuffer(reinterpret_cast<size_t>(&*ptr), buffer + 2);
    char* end = buffer + strlen(buffer);
    memcpy(end, " -> ", 5);
    return buffer + ToString(*ptr);
  } else {
    return "<NULL>";
  }
}

template <class First, class Second>
std::string ToString(const std::pair<First, Second>& pair) {
  return "{" + ToString(pair.first) + ", " + ToString(pair.second) + "}";
}

template<class Tuple, size_t index, bool exist>
class TupleToString {
 public:
  static void Apply(const Tuple& tuple, std::string* out) {
    if (index) {
      *out += ", ";
    }
    *out += ToString(std::get<index>(tuple));
    TupleToString<Tuple, index + 1, (index + 1 < std::tuple_size<Tuple>::value)>::Apply(tuple, out);
  }
};

template<class Tuple, size_t index>
class TupleToString<Tuple, index, false> {
 public:
  static void Apply(const Tuple& tuple, std::string* out) {}
};

template <class... Args>
std::string ToString(const std::tuple<Args...>& tuple) {
  typedef std::tuple<Args...> Tuple;
  std::string result = "{";
  TupleToString<Tuple, 0, (0 < std::tuple_size<Tuple>::value)>::Apply(tuple, &result);
  result += "}";
  return result;
}

template<class Rep, class Period>
std::string ToString(const std::chrono::duration<Rep, Period>& duration) {
  int64_t milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
  int64_t seconds = milliseconds / 1000;
  milliseconds -= seconds * 1000;
  return StringPrintf("%" PRId64 ".%03" PRId64 "s", seconds, milliseconds);
}

std::string ToString(const std::chrono::steady_clock::time_point& time_point);
std::string ToString(const std::chrono::system_clock::time_point& time_point);

template <class Collection>
std::string CollectionToString(const Collection& collection) {
  std::string result = "[";
  auto first = true;
  for (const auto& item : collection) {
    if (first) {
      first = false;
    } else {
      result += ", ";
    }
    result += ToString(item);
  }
  result += "]";
  return result;
}

template <class T>
std::string AsString(const T& t) {
  return ToString(t);
}

} // namespace yb

#if BOOST_PP_VARIADICS

#define YB_FIELD_TO_STRING(r, data, elem) \
    " " BOOST_PP_STRINGIZE(elem) ": " + AsString(BOOST_PP_CAT(elem, BOOST_PP_APPLY(data))) +
#define YB_FIELDS_TO_STRING(data, ...) \
    BOOST_PP_SEQ_FOR_EACH(YB_FIELD_TO_STRING, data(), BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

// This can be used to simplify ToString function implementations in structs where field names do
// not end with an underscore. Suppose we have a struct with fields a and b. If we implement
// ToString as
//
// std::string ToString() const {
//   return YB_STRUCT_TO_STRING(a, b);
// }
//
// we will get ToString return values of the form "{ a: value_for_a b: value_for_b }".
#define YB_STRUCT_TO_STRING(...) \
    "{" YB_FIELDS_TO_STRING(BOOST_PP_NIL, __VA_ARGS__) " }"

// This can be used to simplify ToString function implementations in classes where field names end
// with an underscore. Suppose we have a class with fields a_ and b_. If we implement ToString as
//
// std::string ToString() const {
//   return YB_CLASS_TO_STRING(a, b);
// }
//
// we will get ToString return values of the form "{ a: value_for_a b: value_for_b }".
#define YB_CLASS_TO_STRING(...) \
    "{" YB_FIELDS_TO_STRING((BOOST_PP_IDENTITY(_)), __VA_ARGS__) " }"

#else
#error "Compiler not supported -- BOOST_PP_VARIADICS is not set. See https://bit.ly/2ZF7rTu."
#endif

#endif // YB_UTIL_TOSTRING_H
