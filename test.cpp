// test.cpp
//
// Differential test: FlatHashMap vs std::unordered_map (ground truth).
//
// Runs 1,000,000 random insert/find/erase operations drawn against a
// fixed-size key pool (so collisions, repeat inserts, and
// erase-then-reinsert all happen), for two key types: uint64_t and
// std::string. After every single operation, both containers must
// agree on the operation's return value and on size(). Any mismatch
// prints the operation number, operation type, key, both results, and
// exits with a non-zero status.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "flat_hash_map.hpp"

namespace {

constexpr std::uint64_t kSeed = 0x5EED1234ABCDULL;
constexpr std::size_t kNumOps = 1'000'000;
constexpr std::size_t kPoolSize = 50'000;

template <class K>
K make_key(std::mt19937_64& rng);

template <>
std::uint64_t make_key<std::uint64_t>(std::mt19937_64& rng) {
  return rng();
}

template <>
std::string make_key<std::string>(std::mt19937_64& rng) {
  static const char charset[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  const std::size_t len = 3 + (rng() % 10);
  std::string s;
  s.reserve(len);
  for (std::size_t i = 0; i < len; ++i) {
    s.push_back(charset[rng() % (sizeof(charset) - 1)]);
  }
  return s;
}

[[noreturn]] void fail(std::size_t op_num, const char* op_name,
                        const std::string& key_str,
                        const std::string& detail) {
  std::cerr << "MISMATCH at op " << op_num << " (" << op_name
            << "), key=" << key_str << ": " << detail << "\n";
  std::exit(1);
}

template <class K>
std::string key_to_string(const K& key) {
  std::ostringstream oss;
  oss << key;
  return oss.str();
}

template <class K>
void run_differential_test(const std::string& label, std::uint64_t seed,
                            std::size_t num_ops, std::size_t pool_size) {
  std::cout << "=== Differential test: " << label << " (seed=" << seed
            << ") ===\n";

  std::mt19937_64 rng(seed);
  std::vector<K> pool(pool_size);
  for (auto& k : pool) {
    k = make_key<K>(rng);
  }

  FlatHashMap<K, std::uint64_t> flat;
  std::unordered_map<K, std::uint64_t> ref;

  std::uniform_int_distribution<int> op_dist(0, 2);
  std::uniform_int_distribution<std::size_t> pool_dist(0, pool_size - 1);
  static const char* op_names[3] = {"insert", "find", "erase"};

  for (std::size_t op_num = 0; op_num < num_ops; ++op_num) {
    const int op = op_dist(rng);
    const K& key = pool[pool_dist(rng)];
    const char* op_name = op_names[op];

    if (op == 0) {  // insert
      const std::uint64_t value = rng();
      const bool flat_r = flat.insert(key, value);
      const bool ref_r = ref.insert({key, value}).second;
      if (flat_r != ref_r) {
        std::ostringstream oss;
        oss << "flat=" << flat_r << " ref=" << ref_r;
        fail(op_num, op_name, key_to_string(key), oss.str());
      }
    } else if (op == 1) {  // find
      std::uint64_t* fp = flat.find(key);
      auto it = ref.find(key);
      const bool flat_found = fp != nullptr;
      const bool ref_found = it != ref.end();
      if (flat_found != ref_found) {
        std::ostringstream oss;
        oss << "found: flat=" << flat_found << " ref=" << ref_found;
        fail(op_num, op_name, key_to_string(key), oss.str());
      }
      if (flat_found && ref_found && *fp != it->second) {
        std::ostringstream oss;
        oss << "value: flat=" << *fp << " ref=" << it->second;
        fail(op_num, op_name, key_to_string(key), oss.str());
      }
    } else {  // erase
      const bool flat_r = flat.erase(key);
      const std::size_t ref_r = ref.erase(key);
      if (flat_r != (ref_r != 0)) {
        std::ostringstream oss;
        oss << "flat=" << flat_r << " ref=" << (ref_r != 0);
        fail(op_num, op_name, key_to_string(key), oss.str());
      }
    }

    if (flat.size() != ref.size()) {
      std::ostringstream oss;
      oss << "size: flat=" << flat.size() << " ref=" << ref.size();
      fail(op_num, op_name, key_to_string(key), oss.str());
    }
  }

  std::cout << "PASS: " << num_ops << " ops, final size=" << flat.size()
            << "\n";
}

}  // namespace

int main() {
  std::cout << "Fixed seed: " << kSeed << "\n";

  run_differential_test<std::uint64_t>("uint64_t keys", kSeed, kNumOps,
                                        kPoolSize);
  run_differential_test<std::string>("string keys", kSeed, kNumOps,
                                      kPoolSize);

  std::cout << "All differential tests passed.\n";
  return 0;
}
