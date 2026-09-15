// flat_hash_map.hpp
//
// FlatHashMap<K, V, Hash>: an open-addressing hash map built on two
// parallel arrays (struct-of-arrays) instead of the node-based chaining
// std::unordered_map is required by the standard to use.
//
// ---------------------------------------------------------------------
// Fingerprint scheme (why it exists)
// ---------------------------------------------------------------------
// Every slot has one state byte alongside its (K, V) pair:
//
//   EMPTY     = 0x00  -- never occupied, or vacated by a rehash.
//                        Probing terminates here.
//   TOMBSTONE = 0x01  -- previously occupied, erased since. Probing
//                        skips over these but does not stop at them.
//   OCCUPIED  = 0x80 | fp   where fp is a 7-bit fingerprint taken from
//                        the HIGH bits of the 64-bit finalized hash
//                        (bits 63..57) -- deliberately independent of
//                        the LOW bits used to compute the slot index
//                        (h & (capacity - 1)), so the fingerprint
//                        carries information the index does not.
//
// Setting the top bit of the state byte for OCCUPIED slots is what
// keeps EMPTY/TOMBSTONE unambiguously distinct from every possible
// fingerprint: OCCUPIED bytes always fall in [0x80, 0xFF] (128 values,
// one per possible 7-bit fingerprint), while EMPTY and TOMBSTONE are
// fixed values below 0x80 that no OCCUPIED byte can ever equal.
//
// During a probe, the fingerprint byte is compared first, before any
// key comparison. Two distinct keys land on the same fingerprint only
// 1-in-128 times for a well-distributed hash, so this single-byte
// compare rejects roughly 127 of every 128 non-matching keys without
// ever touching the key itself. That matters most for expensive keys
// like std::string: a real comparison means chasing a pointer to
// separately-allocated heap data and walking bytes, while the state
// array is packed at one byte per slot (many fingerprints share a
// cache line), so the fingerprint check is nearly free by comparison.
// ---------------------------------------------------------------------
//
// Design notes:
//   - Capacity is always a power of two; indexing is h & (capacity-1).
//   - Probing is linear (idx = (idx + 1) & mask).
//   - The table grows (doubles) when size_ + tombstones_ + 1 would
//     exceed 0.75 * capacity_. Counting tombstones toward this trigger
//     (not just live entries) is a deliberate correctness safeguard:
//     with plain linear probing, a probe/insert loop terminates only
//     when it reaches an EMPTY slot. If growth were triggered by live
//     size alone, a long enough sequence of insert/erase churn could
//     fill every slot with OCCUPIED+TOMBSTONE and leave no EMPTY slot
//     at all, which would make find()/insert() loop forever. Doubling
//     also rehashes, and rehashing drops tombstones, so this is the
//     only place tombstones are cleared.
//   - insert() remembers the first TOMBSTONE it passes but keeps
//     probing all the way to an EMPTY slot to confirm the key is truly
//     absent before writing into the remembered tombstone slot.
//   - erase() writes TOMBSTONE, never EMPTY, so later probes for other
//     keys that hashed past this slot are not cut short.
//
// Limitation: K and V must be default constructible. Slots are
// pre-allocated as default-constructed pairs and erase() resets a
// vacated slot to a default-constructed pair to release any resources
// the old key/value held (e.g. a std::string's heap buffer).

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

template <class K, class V, class Hash = std::hash<K>>
class FlatHashMap {
  static_assert(std::is_default_constructible<K>::value,
                "FlatHashMap requires K to be default constructible");
  static_assert(std::is_default_constructible<V>::value,
                "FlatHashMap requires V to be default constructible");

 public:
  FlatHashMap() = default;

  // Inserts (key, value) if key is not already present. Returns true
  // if a new entry was inserted, false if key already existed (in
  // which case the existing value is left untouched, matching
  // std::unordered_map::insert semantics).
  bool insert(const K& key, const V& value) {
    if (needs_grow_before_insert()) {
      grow();
    }

    const std::uint64_t h = hash_key(key);
    const std::uint8_t fp = fingerprint_of(h);
    const std::size_t mask = capacity_ - 1;
    std::size_t idx = h & mask;
    std::size_t first_tombstone = kNoTombstone;

    while (true) {
      ++probe_count_;
      const std::uint8_t st = state_[idx];
      if (st == kEmpty) {
        std::size_t target = idx;
        if (first_tombstone != kNoTombstone) {
          target = first_tombstone;
          --tombstones_;
        }
        slots_[target].first = key;
        slots_[target].second = value;
        state_[target] = static_cast<std::uint8_t>(kOccupiedBit | fp);
        ++size_;
        return true;
      }
      if (st == kTombstone) {
        if (first_tombstone == kNoTombstone) {
          first_tombstone = idx;
        }
      } else if ((st & kFingerprintMask) == fp && slots_[idx].first == key) {
        return false;
      }
      idx = (idx + 1) & mask;
    }
  }

  // Returns a pointer to the value for key, or nullptr if absent.
  V* find(const K& key) {
    if (capacity_ == 0) {
      return nullptr;
    }
    const std::uint64_t h = hash_key(key);
    const std::uint8_t fp = fingerprint_of(h);
    const std::size_t mask = capacity_ - 1;
    std::size_t idx = h & mask;

    while (true) {
      ++probe_count_;
      const std::uint8_t st = state_[idx];
      if (st == kEmpty) {
        return nullptr;
      }
      if (st != kTombstone && (st & kFingerprintMask) == fp &&
          slots_[idx].first == key) {
        return &slots_[idx].second;
      }
      idx = (idx + 1) & mask;
    }
  }

  // Removes key if present. Returns true if an entry was removed.
  bool erase(const K& key) {
    if (capacity_ == 0) {
      return false;
    }
    const std::uint64_t h = hash_key(key);
    const std::uint8_t fp = fingerprint_of(h);
    const std::size_t mask = capacity_ - 1;
    std::size_t idx = h & mask;

    while (true) {
      ++probe_count_;
      const std::uint8_t st = state_[idx];
      if (st == kEmpty) {
        return false;
      }
      if (st != kTombstone && (st & kFingerprintMask) == fp &&
          slots_[idx].first == key) {
        state_[idx] = kTombstone;
        slots_[idx] = std::pair<K, V>{};
        --size_;
        ++tombstones_;
        return true;
      }
      idx = (idx + 1) & mask;
    }
  }

  std::size_t size() const { return size_; }
  std::size_t capacity() const { return capacity_; }

  double load_factor() const {
    return capacity_ == 0
               ? 0.0
               : static_cast<double>(size_) / static_cast<double>(capacity_);
  }

  // Probe instrumentation: total number of slot examinations performed
  // by insert()/find()/erase() (and internal rehashing) since the last
  // reset_probes() call. Used to report probes-per-op in benchmarks.
  std::size_t probe_count() const { return probe_count_; }
  void reset_probes() { probe_count_ = 0; }

 private:
  static constexpr std::uint8_t kEmpty = 0x00;
  static constexpr std::uint8_t kTombstone = 0x01;
  static constexpr std::uint8_t kOccupiedBit = 0x80;
  static constexpr std::uint8_t kFingerprintMask = 0x7F;
  static constexpr std::size_t kInitialCapacity = 16;
  static constexpr double kMaxLoadFactor = 0.75;
  static constexpr std::size_t kNoTombstone =
      static_cast<std::size_t>(-1);

  static std::uint64_t splitmix64_finalize(std::uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
  }

  std::uint64_t hash_key(const K& key) const {
    const std::uint64_t h = static_cast<std::uint64_t>(Hash{}(key));
    return splitmix64_finalize(h);
  }

  static std::uint8_t fingerprint_of(std::uint64_t h) {
    return static_cast<std::uint8_t>((h >> 57) & kFingerprintMask);
  }

  bool needs_grow_before_insert() const {
    if (capacity_ == 0) {
      return true;
    }
    const std::size_t used_after = size_ + tombstones_ + 1;
    return static_cast<double>(used_after) >
           kMaxLoadFactor * static_cast<double>(capacity_);
  }

  void grow() {
    const std::size_t new_capacity =
        (capacity_ == 0) ? kInitialCapacity : capacity_ * 2;
    rehash_to(new_capacity);
  }

  void rehash_to(std::size_t new_capacity) {
    std::vector<std::pair<K, V>> old_slots = std::move(slots_);
    std::vector<std::uint8_t> old_state = std::move(state_);

    slots_.assign(new_capacity, std::pair<K, V>{});
    state_.assign(new_capacity, kEmpty);
    capacity_ = new_capacity;
    tombstones_ = 0;
    size_ = 0;

    const std::size_t mask = capacity_ - 1;
    for (std::size_t i = 0; i < old_state.size(); ++i) {
      if ((old_state[i] & kOccupiedBit) == 0) {
        continue;  // EMPTY or TOMBSTONE: tombstones are dropped here.
      }
      const std::uint64_t h = hash_key(old_slots[i].first);
      const std::uint8_t fp = fingerprint_of(h);
      std::size_t idx = h & mask;
      while (true) {
        ++probe_count_;
        if (state_[idx] == kEmpty) {
          break;
        }
        idx = (idx + 1) & mask;
      }
      slots_[idx] = std::move(old_slots[i]);
      state_[idx] = static_cast<std::uint8_t>(kOccupiedBit | fp);
      ++size_;
    }
  }

  std::vector<std::pair<K, V>> slots_;
  std::vector<std::uint8_t> state_;
  std::size_t size_ = 0;
  std::size_t tombstones_ = 0;
  std::size_t capacity_ = 0;
  std::size_t probe_count_ = 0;
};
