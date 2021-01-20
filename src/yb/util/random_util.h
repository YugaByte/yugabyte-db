// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#ifndef YB_UTIL_RANDOM_UTIL_H
#define YB_UTIL_RANDOM_UTIL_H

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <string>

#include <glog/logging.h>

namespace yb {

class Random;

// Writes exactly n random bytes to dest using the parameter Random generator.
// Note RandomString() does not null-terminate its strings, though '\0' could
// be written to dest with the same probability as any other byte.
void RandomString(void* dest, size_t n, Random* rng);

// Generate a 32-bit random seed from several sources, including timestamp,
// pid & tid.
uint32_t GetRandomSeed32();

std::vector<uint8_t> RandomBytes(size_t len, std::mt19937_64* rng = nullptr);

std::string RandomHumanReadableString(int len, Random* rnd);

class RandomDeviceSequence {
 public:
  typedef std::random_device::result_type result_type;

  template<class It>
  void generate(It begin, const It& end) {
    std::generate(begin, end, [this] { return device_(); });
  }
 private:
  std::random_device device_;
};

// Correct seeding of random number generator.
// It is quite futile to use 32bit seed for generator with 19968bit state, like mt19937.
template<class Engine>
void Seed(Engine* engine) {
  RandomDeviceSequence sequence;
  engine->seed(sequence);
}

std::mt19937_64& ThreadLocalRandom();

template <class Int>
Int RandomUniformInt(Int min, Int max, std::mt19937_64* rng = nullptr) {
  if (!rng) {
    rng = &ThreadLocalRandom();
  }
  return std::uniform_int_distribution<Int>(min, max)(*rng);
}

bool RandomUniformBool(std::mt19937_64* rng = nullptr);

template <class Int>
std::vector<Int> RandomUniformVector(Int min, Int max, uint32_t size,
                                     std::mt19937_64* rng = nullptr) {
  std::vector<Int> vec(size);
  std::generate(vec.begin(), vec.end(), [=] { return RandomUniformInt(min, max, rng); });
  return vec;
}

template <class Int>
bool RandomWithChance(Int chance, std::mt19937_64* rng = nullptr) {
  if (!rng) {
    rng = &ThreadLocalRandom();
  }
  return RandomUniformInt(static_cast<Int>(0), chance - 1, rng) == 0;
}

template <class Int>
Int RandomUniformInt(std::mt19937_64* rng = nullptr) {
  typedef std::numeric_limits<Int> Limits;
  return RandomUniformInt<Int>(Limits::min(), Limits::max(), rng);
}

template <class Real>
Real RandomUniformReal(Real a, Real b, std::mt19937_64* rng = nullptr) {
  if (!rng) {
    rng = &ThreadLocalRandom();
  }
  return std::uniform_real_distribution<Real>(a, b)(*rng);
}

template <class Real>
Real RandomUniformReal(std::mt19937_64* rng = nullptr) {
  return RandomUniformReal(static_cast<Real>(0), static_cast<Real>(1), rng);
}

inline bool RandomActWithProbability(double probability, std::mt19937_64* rng = nullptr) {
  return probability <= 0 ? false
                          : probability >= 1.0 ? true
                                               : RandomUniformReal<double>(rng) < probability;
}

template <class Collection>
typename Collection::const_reference RandomElement(const Collection& collection,
                                                   std::mt19937_64* rng = nullptr) {
  CHECK(!collection.empty());
  size_t index = RandomUniformInt<size_t>(0, collection.size() - 1, rng);
  auto it = collection.begin();
  std::advance(it, index);
  return *it;
}

std::string RandomHumanReadableString(size_t len, std::mt19937_64* rng = nullptr);

} // namespace yb

#endif // YB_UTIL_RANDOM_UTIL_H
