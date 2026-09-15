// bench.cpp
//
// Benchmarks FlatHashMap against std::unordered_map on two workloads:
//
//   1. Integer keys (random uint64_t), N swept over 1e3..1e7. Timed
//      phases: insert, lookup-hit, lookup-miss, erase.
//   2. String keys: word-frequency count over a text file (argv[1]).
//      Only the insert-or-increment loop is timed; tokenization and
//      all I/O happen before any timer starts.
//
// Methodology:
//   - std::chrono::steady_clock only.
//   - The key array (and, for workload 1, the miss-key array) is
//     generated once and the identical array is reused for both
//     container types.
//   - Neither container is ever given a reserve() call/hint.
//   - Each (N, container) cell runs 5 full trials. A trial builds a
//     fresh, empty container and times insert, lookup-hit,
//     lookup-miss, and erase against it in sequence, so setup work is
//     never duplicated across phases within a trial. The first trial
//     is discarded as warmup; the reported number is the median of
//     the remaining 4 samples, per phase.
//   - Values read back by lookups are folded into a checksum that is
//     printed, so -O2 cannot prove the lookup loops are dead and
//     delete them.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "flat_hash_map.hpp"

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_ns(Clock::time_point t0, Clock::time_point t1) {
  return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n == 0) return 0.0;
  if (n % 2 == 1) return v[n / 2];
  return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

// ---------------------------------------------------------------------
// Container-agnostic op adapters. Overload resolution picks the right
// one for FlatHashMap<K,V> vs std::unordered_map<K,V>, so the trial
// loop below is written once and shared by both container types.
// ---------------------------------------------------------------------

template <class K, class V, class Hash>
bool op_insert(FlatHashMap<K, V, Hash>& c, const K& k, const V& v) {
  return c.insert(k, v);
}
template <class K, class V>
bool op_insert(std::unordered_map<K, V>& c, const K& k, const V& v) {
  return c.insert({k, v}).second;
}

template <class K, class V, class Hash>
V* op_find(FlatHashMap<K, V, Hash>& c, const K& k) {
  return c.find(k);
}
template <class K, class V>
V* op_find(std::unordered_map<K, V>& c, const K& k) {
  auto it = c.find(k);
  return it == c.end() ? nullptr : &it->second;
}

template <class K, class V, class Hash>
bool op_erase(FlatHashMap<K, V, Hash>& c, const K& k) {
  return c.erase(k);
}
template <class K, class V>
bool op_erase(std::unordered_map<K, V>& c, const K& k) {
  return c.erase(k) != 0;
}

template <class K, class V, class Hash>
std::size_t op_probes(FlatHashMap<K, V, Hash>& c) {
  return c.probe_count();
}
template <class K, class V>
std::size_t op_probes(std::unordered_map<K, V>&) {
  return 0;
}

template <class K, class V, class Hash>
void op_reset_probes(FlatHashMap<K, V, Hash>& c) {
  c.reset_probes();
}
template <class K, class V>
void op_reset_probes(std::unordered_map<K, V>&) {}

struct PhaseResult {
  double median_ns_per_op = 0.0;
  double median_probes_per_op = 0.0;
};

struct TrialResults {
  PhaseResult insert;
  PhaseResult lookup_hit;
  PhaseResult lookup_miss;
  PhaseResult erase;
};

// Runs kRuns trials of insert/lookup-hit/lookup-miss/erase against a
// freshly constructed Container each time, and returns the per-phase
// median (first trial discarded as warmup).
template <class Container, class K, class V>
TrialResults run_int_workload_trials(const std::vector<K>& keys,
                                      const std::vector<K>& miss_keys,
                                      std::uint64_t& global_checksum) {
  constexpr int kRuns = 5;
  const std::size_t n = keys.size();

  std::vector<double> insert_ns, hit_ns, miss_ns, erase_ns;
  std::vector<double> insert_pp, hit_pp, miss_pp, erase_pp;

  for (int run = 0; run < kRuns; ++run) {
    Container c;
    std::uint64_t checksum = 0;

    op_reset_probes(c);
    auto t0 = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      op_insert(c, keys[i], static_cast<V>(keys[i]));
    }
    auto t1 = Clock::now();
    insert_ns.push_back(elapsed_ns(t0, t1) / static_cast<double>(n));
    insert_pp.push_back(static_cast<double>(op_probes(c)) /
                         static_cast<double>(n));

    op_reset_probes(c);
    t0 = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      V* p = op_find(c, keys[i]);
      if (p) checksum += *p;
    }
    t1 = Clock::now();
    hit_ns.push_back(elapsed_ns(t0, t1) / static_cast<double>(n));
    hit_pp.push_back(static_cast<double>(op_probes(c)) /
                      static_cast<double>(n));

    op_reset_probes(c);
    t0 = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      V* p = op_find(c, miss_keys[i]);
      if (p) checksum += *p;
    }
    t1 = Clock::now();
    miss_ns.push_back(elapsed_ns(t0, t1) / static_cast<double>(n));
    miss_pp.push_back(static_cast<double>(op_probes(c)) /
                       static_cast<double>(n));

    op_reset_probes(c);
    t0 = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
      op_erase(c, keys[i]);
    }
    t1 = Clock::now();
    erase_ns.push_back(elapsed_ns(t0, t1) / static_cast<double>(n));
    erase_pp.push_back(static_cast<double>(op_probes(c)) /
                        static_cast<double>(n));

    global_checksum += checksum;
  }

  auto drop_warmup = [](std::vector<double>& v) {
    v.erase(v.begin());
  };
  drop_warmup(insert_ns);
  drop_warmup(hit_ns);
  drop_warmup(miss_ns);
  drop_warmup(erase_ns);
  drop_warmup(insert_pp);
  drop_warmup(hit_pp);
  drop_warmup(miss_pp);
  drop_warmup(erase_pp);

  TrialResults r;
  r.insert = {median_of(insert_ns), median_of(insert_pp)};
  r.lookup_hit = {median_of(hit_ns), median_of(hit_pp)};
  r.lookup_miss = {median_of(miss_ns), median_of(miss_pp)};
  r.erase = {median_of(erase_ns), median_of(erase_pp)};
  return r;
}

void print_and_record_row(std::ofstream& csv, const std::string& workload,
                           std::size_t n, const std::string& container,
                           const std::string& phase, double median_ns,
                           double probes_per_op) {
  std::cout << "  " << container << " / " << phase << ": " << median_ns
            << " ns/op";
  if (probes_per_op > 0.0) {
    std::cout << ", " << probes_per_op << " probes/op";
  }
  std::cout << "\n";
  csv << workload << "," << n << "," << container << "," << phase << ","
      << median_ns << "," << probes_per_op << "\n";
}

void run_integer_workload(std::ofstream& csv, std::uint64_t seed) {
  const std::vector<std::size_t> sizes = {1'000, 10'000, 100'000, 1'000'000,
                                           10'000'000};

  for (std::size_t n : sizes) {
    std::cout << "--- Integer workload, N=" << n << " ---\n";

    std::mt19937_64 rng(seed + n);
    std::vector<std::uint64_t> keys(n);
    std::vector<std::uint64_t> miss_keys(n);
    // High bit forced to 0 for present keys, 1 for absent keys, so the
    // two sets can never collide (a hard guarantee, not a probabilistic
    // one), while still drawing from 63 bits of entropy each.
    for (std::size_t i = 0; i < n; ++i) {
      keys[i] = rng() & ~(std::uint64_t{1} << 63);
      miss_keys[i] = rng() | (std::uint64_t{1} << 63);
    }

    std::uint64_t checksum = 0;

    {
      using Container = FlatHashMap<std::uint64_t, std::uint64_t>;
      TrialResults r =
          run_int_workload_trials<Container, std::uint64_t, std::uint64_t>(
              keys, miss_keys, checksum);
      print_and_record_row(csv, "int", n, "FlatHashMap", "insert",
                            r.insert.median_ns_per_op,
                            r.insert.median_probes_per_op);
      print_and_record_row(csv, "int", n, "FlatHashMap", "lookup_hit",
                            r.lookup_hit.median_ns_per_op,
                            r.lookup_hit.median_probes_per_op);
      print_and_record_row(csv, "int", n, "FlatHashMap", "lookup_miss",
                            r.lookup_miss.median_ns_per_op,
                            r.lookup_miss.median_probes_per_op);
      print_and_record_row(csv, "int", n, "FlatHashMap", "erase",
                            r.erase.median_ns_per_op,
                            r.erase.median_probes_per_op);
    }
    {
      using Container = std::unordered_map<std::uint64_t, std::uint64_t>;
      TrialResults r =
          run_int_workload_trials<Container, std::uint64_t, std::uint64_t>(
              keys, miss_keys, checksum);
      print_and_record_row(csv, "int", n, "unordered_map", "insert",
                            r.insert.median_ns_per_op, 0.0);
      print_and_record_row(csv, "int", n, "unordered_map", "lookup_hit",
                            r.lookup_hit.median_ns_per_op, 0.0);
      print_and_record_row(csv, "int", n, "unordered_map", "lookup_miss",
                            r.lookup_miss.median_ns_per_op, 0.0);
      print_and_record_row(csv, "int", n, "unordered_map", "erase",
                            r.erase.median_ns_per_op, 0.0);
    }

    std::cout << "  checksum=" << checksum << "\n";
  }
}

std::vector<std::string> tokenize_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("could not open file: " + path);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();

  std::vector<std::string> tokens;
  std::string current;
  for (unsigned char ch : text) {
    if (std::isalnum(ch)) {
      current.push_back(static_cast<char>(std::tolower(ch)));
    } else if (!current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

template <class Container>
double run_string_workload_trial(const std::vector<std::string>& tokens,
                                  std::size_t& probes_out) {
  Container c;
  op_reset_probes(c);
  auto t0 = Clock::now();
  for (const auto& word : tokens) {
    std::uint64_t* p = op_find(c, word);
    if (p) {
      ++(*p);
    } else {
      op_insert(c, word, std::uint64_t{1});
    }
  }
  auto t1 = Clock::now();
  probes_out = op_probes(c);
  return elapsed_ns(t0, t1) / static_cast<double>(tokens.size());
}

void run_string_workload(std::ofstream& csv, const std::string& path) {
  std::cout << "--- String workload: word frequency over \"" << path
            << "\" ---\n";
  std::vector<std::string> tokens = tokenize_file(path);
  const std::size_t n = tokens.size();
  std::cout << "  tokens: " << n << "\n";

  constexpr int kRuns = 5;

  {
    std::vector<double> ns_samples;
    std::vector<double> probe_samples;
    for (int run = 0; run < kRuns; ++run) {
      std::size_t probes = 0;
      double ns = run_string_workload_trial<
          FlatHashMap<std::string, std::uint64_t>>(tokens, probes);
      ns_samples.push_back(ns);
      probe_samples.push_back(static_cast<double>(probes) /
                               static_cast<double>(n));
    }
    ns_samples.erase(ns_samples.begin());
    probe_samples.erase(probe_samples.begin());
    print_and_record_row(csv, "string_wordfreq", n, "FlatHashMap",
                          "insert_or_increment", median_of(ns_samples),
                          median_of(probe_samples));
  }
  {
    std::vector<double> ns_samples;
    for (int run = 0; run < kRuns; ++run) {
      std::size_t probes = 0;
      double ns = run_string_workload_trial<
          std::unordered_map<std::string, std::uint64_t>>(tokens, probes);
      ns_samples.push_back(ns);
    }
    ns_samples.erase(ns_samples.begin());
    print_and_record_row(csv, "string_wordfreq", n, "unordered_map",
                          "insert_or_increment", median_of(ns_samples), 0.0);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::cout << "Compiler: " << __VERSION__ << "\n";
  std::cout << "Build flags: -O2 -std=c++17 -Wall -Wextra\n";
  std::cout << "sizeof(std::pair<uint64_t,uint64_t>) = "
            << sizeof(std::pair<std::uint64_t, std::uint64_t>) << "\n";
  std::cout << "sizeof(std::pair<std::string,uint64_t>) = "
            << sizeof(std::pair<std::string, std::uint64_t>) << "\n";
  std::cout << "\n";

  constexpr std::uint64_t kSeed = 0x5EED1234ABCDULL;

  std::ofstream csv("results.csv");
  csv << "workload,N,container,phase,median_ns_per_op,probes_per_op\n";

  run_integer_workload(csv, kSeed);

  std::cout << "\n";
  if (argc >= 2) {
    run_string_workload(csv, argv[1]);
  } else {
    std::cout << "No text file given as argv[1]; skipping string workload.\n"
                 "Usage: ./bench <path-to-text-file>\n";
  }

  std::cout << "\nWrote results.csv\n";
  return 0;
}
