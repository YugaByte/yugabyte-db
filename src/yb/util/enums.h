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

#ifndef YB_UTIL_ENUMS_H_
#define YB_UTIL_ENUMS_H_

#include <bitset>
#include <string>

#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/expr_if.hpp>
#include <boost/preprocessor/if.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/facilities/apply.hpp>
#include <boost/preprocessor/punctuation/is_begin_parens.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/seq/fold_left.hpp>

#include <boost/core/demangle.hpp>

#include <glog/logging.h>

#include "yb/util/math_util.h"

namespace yb {

// Convert a strongly typed enum to its underlying type.
// Based on an answer to this StackOverflow question: https://goo.gl/zv2Wg3
template <typename E>
constexpr typename std::underlying_type<E>::type to_underlying(E e) {
  return static_cast<typename std::underlying_type<E>::type>(e);
}

// YB_DEFINE_ENUM
// -----------------------------------------------------------------------------------------------

// A convenient way to define enums along with string conversion functions.
// Example:
//
//   YB_DEFINE_ENUM(MyEnum, (kFoo)(kBar)(kBaz))
//
// This will define
// - An enum class MyEnum with values FOO, BAR, and BAZ.
// - A ToString() function converting a value of MyEnum to std::string, including a diagnostic
//   string for invalid values.
// - A stream output operator for MyEnum using the above ToString function.
// - A ToCString() function converting an enum value to a C string, or nullptr for invalid values.

#define YB_ENUM_ITEM_NAME(elem) \
    BOOST_PP_IF(BOOST_PP_IS_BEGIN_PARENS(elem), BOOST_PP_TUPLE_ELEM(2, 0, elem), elem)

#define YB_ENUM_ITEM_VALUE(elem) \
    BOOST_PP_EXPR_IF(BOOST_PP_IS_BEGIN_PARENS(elem), = BOOST_PP_TUPLE_ELEM(2, 1, elem))

#define YB_ENUM_ITEM(s, data, elem) \
    BOOST_PP_CAT(BOOST_PP_APPLY(data), YB_ENUM_ITEM_NAME(elem)) YB_ENUM_ITEM_VALUE(elem),

#define YB_ENUM_LIST_ITEM(s, data, elem) \
    BOOST_PP_TUPLE_ELEM(2, 0, data):: \
        BOOST_PP_CAT(BOOST_PP_APPLY(BOOST_PP_TUPLE_ELEM(2, 1, data)), YB_ENUM_ITEM_NAME(elem)),

#define YB_ENUM_CASE_NAME(s, data, elem) \
  case BOOST_PP_TUPLE_ELEM(2, 0, data):: \
      BOOST_PP_CAT(BOOST_PP_APPLY(BOOST_PP_TUPLE_ELEM(2, 1, data)), YB_ENUM_ITEM_NAME(elem)): \
          return BOOST_PP_STRINGIZE(YB_ENUM_ITEM_NAME(elem));

#define YB_ENUM_MAX_ENUM_NAME(enum_name, prefix, value) enum_name
#define YB_ENUM_MAX_PREFIX(enum_name, prefix, value) prefix
#define YB_ENUM_MAX_VALUE(enum_name, prefix, value) value
#define YB_ENUM_MAX_OP(s, data, x) \
    (YB_ENUM_MAX_ENUM_NAME data, \
     YB_ENUM_MAX_PREFIX data, \
     yb::constexpr_max(YB_ENUM_MAX_VALUE data, YB_ENUM_MAX_ENUM_NAME data::YB_ENUM_ITEM_NAME(x)))

#define YB_DEFINE_ENUM_IMPL(enum_name, prefix, list) \
  enum class enum_name { \
    BOOST_PP_SEQ_FOR_EACH(YB_ENUM_ITEM, prefix, list) \
  }; \
  \
  inline __attribute__((unused)) const char* ToCString(enum_name value) { \
    switch(value) { \
    BOOST_PP_SEQ_FOR_EACH(YB_ENUM_CASE_NAME, (enum_name, prefix), list); \
    } \
    return nullptr; \
  } \
  \
  inline __attribute__((unused)) std::string ToString(enum_name value) { \
    const char* c_str = ToCString(value); \
    if (c_str != nullptr) \
      return c_str; \
    return "<unknown " BOOST_PP_STRINGIZE(enum_name) " : " + \
           std::to_string(::yb::to_underlying(value)) + ">"; \
  } \
  inline __attribute__((unused)) std::ostream& operator<<(std::ostream& out, enum_name value) { \
    return out << ToString(value); \
  } \
  \
  constexpr __attribute__((unused)) size_t BOOST_PP_CAT(kElementsIn, enum_name) = \
      BOOST_PP_SEQ_SIZE(list); \
  constexpr __attribute__((unused)) size_t BOOST_PP_CAT(k, BOOST_PP_CAT(enum_name, MapSize)) = \
      static_cast<size_t>(BOOST_PP_TUPLE_ELEM(3, 2, \
          BOOST_PP_SEQ_FOLD_LEFT( \
              YB_ENUM_MAX_OP, \
              (enum_name, prefix, enum_name::YB_ENUM_ITEM_NAME(BOOST_PP_SEQ_HEAD(list))), \
              BOOST_PP_SEQ_TAIL(list)))) + 1; \
  constexpr __attribute__((unused)) std::initializer_list<enum_name> \
      BOOST_PP_CAT(k, BOOST_PP_CAT(enum_name, List)) = {\
          BOOST_PP_SEQ_FOR_EACH(YB_ENUM_LIST_ITEM, (enum_name, prefix), list) \
  };\
  /* Functions returning kEnumMapSize and kEnumList that could be used in templates. */ \
  constexpr __attribute__((unused)) size_t MapSize(enum_name*) { \
    return BOOST_PP_CAT(k, BOOST_PP_CAT(enum_name, MapSize)); \
  } \
  constexpr __attribute__((unused)) auto List(enum_name*) { \
    return BOOST_PP_CAT(k, BOOST_PP_CAT(enum_name, List)); \
  } \
  /**/

// Please see the usage of YB_DEFINE_ENUM before the auxiliary macros above.
#define YB_DEFINE_ENUM(enum_name, list) YB_DEFINE_ENUM_IMPL(enum_name, BOOST_PP_NIL, list)
#define YB_DEFINE_ENUM_EX(enum_name, prefix, list) YB_DEFINE_ENUM_IMPL(enum_name, (prefix), list)

// This macro can be used after exhaustive (compile-time-checked) switches on enums without a
// default clause to handle invalid values due to memory corruption.
//
// switch (my_enum_value) {
//   case MyEnum::FOO:
//     // some handling
//     return;
//   . . .
//   case MyEnum::BAR:
//     // some handling
//     return;
// }
// FATAL_INVALID_ENUM_VALUE(MyEnum, my_enum_value);
//
// This uses a function marked with [[noreturn]] so that the compiler will not complain about
// functions not returning a value.
//
// We need to specify the enum name because there does not seem to be an non-RTTI way to get
// a type name string from a type in a template.
#define FATAL_INVALID_ENUM_VALUE(enum_type, value_macro_arg) \
    do { \
      auto _value_copy = (value_macro_arg); \
      static_assert( \
          std::is_same<decltype(_value_copy), enum_type>::value, \
          "Type of enum value passed to FATAL_INVALID_ENUM_VALUE must be " \
          BOOST_PP_STRINGIZE(enum_type)); \
      ::yb::FatalInvalidEnumValueInternal<enum_type>( \
          BOOST_PP_STRINGIZE(enum_type), std::string(), _value_copy, \
          BOOST_PP_STRINGIZE(value_macro_arg), __FILE__, __LINE__); \
    } while (0)

#define FATAL_INVALID_PB_ENUM_VALUE(enum_type, value_macro_arg) \
    do { \
      auto _value_copy = (value_macro_arg); \
      static_assert( \
          std::is_same<decltype(_value_copy), enum_type>::value, \
          "Type of enum value passed to FATAL_INVALID_ENUM_VALUE must be " \
          BOOST_PP_STRINGIZE(enum_type)); \
      ::yb::FatalInvalidEnumValueInternal<enum_type>( \
          BOOST_PP_STRINGIZE(enum_type), BOOST_PP_CAT(enum_type, _Name)(_value_copy), _value_copy, \
          BOOST_PP_STRINGIZE(value_macro_arg), __FILE__, __LINE__); \
    } while (0)

template<typename T>
std::string GetTypeName() {
  char const* type_name = typeid(T).name();
  boost::core::scoped_demangled_name type_name_demangled(type_name);

  // From https://stackoverflow.com/questions/1488186/stringifying-template-arguments:
  return type_name_demangled.get() ? type_name_demangled.get() : type_name;
}

template<typename Enum>
[[noreturn]] void FatalInvalidEnumValueInternal(
    const char* enum_name,
    const std::string& value_str,
    Enum value,
    const char* expression_str,
    const char* fname,
    int line) {
  google::LogMessageFatal(fname, line).stream()
      << "Invalid value of enum " << enum_name << " ("
      << "full enum type: " << GetTypeName<Enum>() << ", "
      << "expression: " << expression_str << "): "
      << value_str << (!value_str.empty() ? " (" : "")
      << std::to_string(to_underlying(value))
      << (!value_str.empty() ? ")" : "") << ".";
  abort();  // Never reached.
}

struct EnumHash {
  template <class T>
  size_t operator()(T t) const {
    return to_underlying(t);
  }
};

// ------------------------------------------------------------------------------------------------
// Enum bit set
// ------------------------------------------------------------------------------------------------

template <class Enum>
class EnumBitSetIterator {
 public:
  typedef typename decltype(List(static_cast<Enum*>(nullptr)))::const_iterator ImplIterator;
  typedef std::bitset<MapSize(static_cast<Enum*>(nullptr))> BitSet;

  EnumBitSetIterator(ImplIterator iter, const BitSet* set) : iter_(iter), set_(set) {
    FindSetBit();
  }

  Enum operator*() const {
    return *iter_;
  }

  EnumBitSetIterator& operator++() {
    ++iter_;
    FindSetBit();
    return *this;
  }

  EnumBitSetIterator operator++(int) {
    EnumBitSetIterator result(*this);
    ++(*this);
    return result;
  }

 private:
  void FindSetBit() {
    while (iter_ != List(static_cast<Enum*>(nullptr)).end() && !set_->test(to_underlying(*iter_))) {
      ++iter_;
    }
  }

  friend bool operator!=(const EnumBitSetIterator<Enum>& lhs, const EnumBitSetIterator<Enum>& rhs) {
    return lhs.iter_ != rhs.iter_;
  }

  ImplIterator iter_;
  const BitSet* set_;
};

// EnumBitSet wraps std::bitset for enum type, to avoid casting to/from underlying type for each
// operation. Also adds type safety.
template <class Enum>
class EnumBitSet {
 public:
  typedef EnumBitSetIterator<Enum> const_iterator;

  EnumBitSet() = default;
  explicit EnumBitSet(uint64_t value) : impl_(value) {}

  explicit EnumBitSet(const std::initializer_list<Enum>& inp) {
    for (auto i : inp) {
      impl_.set(to_underlying(i));
    }
  }

  bool Test(Enum value) const {
    return impl_.test(to_underlying(value));
  }

  uintptr_t ToUIntPtr() const {
    return impl_.to_ulong();
  }

  bool None() const {
    return impl_.none();
  }

  bool Any() const {
    return impl_.any();
  }

  bool All() const {
    return impl_.all();
  }

  EnumBitSet& Set(Enum value) {
    impl_.set(to_underlying(value));
    return *this;
  }

  const_iterator begin() const {
    return const_iterator(List(static_cast<Enum*>(nullptr)).begin(), &impl_);
  }

  const_iterator end() const {
    return const_iterator(List(static_cast<Enum*>(nullptr)).end(), &impl_);
  }

  EnumBitSet<Enum>& operator|=(const EnumBitSet& rhs) {
    impl_ |= rhs.impl_;
    return *this;
  }

  EnumBitSet<Enum>& operator&=(const EnumBitSet& rhs) {
    impl_ &= rhs.impl_;
    return *this;
  }

  bool operator==(const EnumBitSet<Enum>& rhs) const {
    return impl_ == rhs.impl_;
  }

  bool operator!=(const EnumBitSet<Enum>& rhs) const {
    return impl_ != rhs.impl_;
  }

  bool operator<(const EnumBitSet<Enum>& rhs) const {
    return impl_.to_ullong() < rhs.impl_.to_ullong();
  }

  bool operator>(const EnumBitSet<Enum>& rhs) const {
    return impl_.to_ullong() > rhs.impl_.to_ullong();
  }

 private:
  std::bitset<MapSize(static_cast<Enum*>(nullptr))> impl_;

  friend EnumBitSet<Enum> operator&(const EnumBitSet& lhs, const EnumBitSet& rhs) {
    EnumBitSet<Enum> result;
    result.impl_ = lhs.impl_ & rhs.impl_;
    return result;
  }

  friend EnumBitSet<Enum> operator|(const EnumBitSet& lhs, const EnumBitSet& rhs) {
    EnumBitSet<Enum> result;
    result.impl_ = lhs.impl_ | rhs.impl_;
    return result;
  }
};

}  // namespace yb

#endif  // YB_UTIL_ENUMS_H_
