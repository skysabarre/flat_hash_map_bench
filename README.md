# FlatHashMap vs std::unordered_map

A hand-written open-addressing hash map (`FlatHashMap<K, V, Hash>`),
differentially tested against `std::unordered_map` and benchmarked
against it on integer and string workloads.

## Files

- `flat_hash_map.hpp` — the map itself, header-only.
- `test.cpp` — differential correctness test vs `std::unordered_map`.
- `bench.cpp` — benchmarks, writes `results.csv`.
- `sample.txt` — a small synthetic text file for the string benchmark
  (any text file works; pass its path as `argv[1]` to `bench`).

## Design decisions

- **Layout**: parallel arrays — `std::vector<std::pair<K,V>> slots_`
  and `std::vector<uint8_t> state_` — rather than a single array of
  structs. The state array is 1 byte/slot, so a probe sequence walks a
  compact, cache-friendly strip of metadata before ever touching the
  (larger, heap-owning-if-K-is-string) slot data.

- **State encoding**: `EMPTY = 0x00`, `TOMBSTONE = 0x01`, and
  `OCCUPIED = 0x80 | fingerprint`, where `fingerprint` is 7 bits taken
  from the high bits of the finalized 64-bit hash (bits 63..57),
  independent of the low bits used for the slot index. Because
  `OCCUPIED` always has its top bit set, it occupies the byte range
  `[0x80, 0xFF]` — 128 values, one per possible fingerprint — which
  can never collide with the two fixed sentinel values below `0x80`.
  See the comment block at the top of `flat_hash_map.hpp` for the full
  rationale: the fingerprint lets a probe reject ~127/128 non-matching
  keys with a single byte compare, without touching the key or calling
  `operator==`, which matters most for `std::string` keys where a real
  comparison means chasing a heap pointer.

- **Hashing**: `std::hash<K>` output is run through a splitmix64
  finalizer before use, so the low bits used for indexing and the high
  bits used for the fingerprint are both well-mixed, independent of
  whatever bit-distribution quirks `std::hash<K>` has on its own
  (notably `std::hash<uint64_t>` is often the identity function on
  common standard library implementations).

- **Growth**: capacity is always a power of two; doubling is
  triggered when `size_ + tombstones_ + 1` would exceed
  `0.75 * capacity_`. Tombstones count toward this on purpose — with
  linear probing, a probe loop only terminates at an `EMPTY` slot, so
  a table that tracked live size alone could fill entirely with
  `OCCUPIED`/`TOMBSTONE` and no `EMPTY` slot left, which would make
  `find()`/`insert()` loop forever. Growing is also the only place
  tombstones are dropped: rehashing copies live entries into a fresh
  array and recomputes every index; tombstones are simply not copied.

- **Insert probing**: `insert()` remembers the index of the first
  `TOMBSTONE` it passes but keeps probing all the way to an `EMPTY`
  slot before concluding the key is absent — this is required for
  correctness (the key could still appear later in the same probe
  chain), then writes into the remembered tombstone slot if one was
  seen (reusing it), or the terminating `EMPTY` slot otherwise.

- **API surface**: intentionally minimal —
  `insert(const K&, const V&) -> bool`, `find(const K&) -> V*`,
  `erase(const K&) -> bool`, `size()`, `capacity()`, `load_factor()`,
  plus `probe_count()` / `reset_probes()` for instrumentation. No
  iterators, no `operator[]`, no `emplace`, no allocator support — by
  design, not by omission.

- **Probe instrumentation**: every slot examined by `insert()`,
  `find()`, `erase()`, or the internal rehash loop increments a
  counter, readable via `probe_count()` and resettable via
  `reset_probes()`. The benchmark resets it before each timed phase
  and reports probes-per-op. Note that an `insert` phase's
  probes-per-op includes any rehashing triggered during that phase —
  rehashing re-probes every live entry into the new table, so an
  insert sequence that crosses a growth threshold several times will
  show a higher probes/op than the steady-state (already-sized) case;
  this is intentional, since rehashing is a real, amortized cost of
  inserting.

## Build and run

```sh
g++ -O2 -std=c++17 -Wall -Wextra -o test  test.cpp
g++ -O2 -std=c++17 -Wall -Wextra -o bench bench.cpp

./test                    # differential correctness test
./bench sample.txt        # full benchmark; writes results.csv
```

`bench` also runs (and prints) the integer-key workload with no
argument at all; the string-key word-frequency workload is skipped if
no file path is given.

## Results (integer keys)

Measured with `./bench sample.txt` — Apple LLVM 16.0.0 (clang-1600.0.26.6),
`-O2 -std=c++17 -Wall -Wextra`, `sizeof(std::pair<uint64_t,uint64_t>) = 16`.
`N` sweeps 1e3..1e7; each cell is the median of 4 timed runs (5 run, first
discarded as warmup). Re-run `./bench` on your own machine to reproduce —
numbers will shift with CPU, allocator, and cache size.

| N | Container | Insert (ns/op) | Lookup-hit (ns/op) | Lookup-miss (ns/op) | Erase (ns/op) | Probes/op (Flat only) |
|---|---|---|---|---|---|---|
| 1e3 | FlatHashMap | 109.17 | 22.87 | 35.81 | 25.40 | 5.19 (insert) / 1.43 (hit,erase) / 2.21 (miss) |
| 1e3 | unordered_map | 160.15 | 31.73 | 42.02 | 134.54 | — |
| 1e4 | FlatHashMap | 54.05 | 15.85 | 24.96 | 18.62 | 4.92 / 1.78 / 3.71 |
| 1e4 | unordered_map | 89.55 | 19.96 | 32.29 | 78.84 | — |
| 1e5 | FlatHashMap | 50.56 | 8.05 | 12.67 | 8.57 | 6.17 / 1.30 / 1.81 |
| 1e5 | unordered_map | 31.10 | 8.60 | 14.00 | 28.33 | — |
| 1e6 | FlatHashMap | 29.90 | 10.70 | 11.64 | 11.91 | 5.40 / 1.45 / 2.33 |
| 1e6 | unordered_map | 114.23 | 18.56 | 28.17 | 99.71 | — |
| 1e7 | FlatHashMap | 29.30 | 19.46 | 20.51 | 21.95 | 4.89 / 1.74 / 3.56 |
| 1e7 | unordered_map | 219.14 | 28.08 | 48.64 | 162.29 | — |

## Results (string keys)

Word-frequency insert-or-increment loop over `sample.txt` (a synthetic,
Zipfian-distributed 300,000-token file bundled in this repo).

| File | Tokens | Container | Insert-or-increment (ns/op) | Probes/op (Flat only) |
|---|---|---|---|---|
| sample.txt | 300,000 | FlatHashMap | 15.34 | 1.28 |
| sample.txt | 300,000 | unordered_map | 17.98 | — |

## Analysis

**Insert and erase are where FlatHashMap wins big**, and the gap widens
with `N`: at 1e7, insert is ~7.5x faster (29ns vs 219ns) and erase is
~7.4x faster (22ns vs 162ns). Both operations on `std::unordered_map`
pay for a heap allocation or deallocation per call; `FlatHashMap` never
allocates on `insert`/`erase` after its backing arrays are sized (it
just writes into an already-allocated slot or flips a state byte to
`TOMBSTONE`). That per-call allocator round-trip is the single biggest
cost `std::unordered_map` carries that `FlatHashMap` structurally can't
have.

**Lookups are closer**, and FlatHashMap's edge shrinks (sometimes
inverts, e.g. `N=1e5`) at mid-range `N`. A hit or miss on
`std::unordered_map` is one hash + one bucket-pointer chase + at most a
couple of node comparisons — already cheap — while `FlatHashMap`'s
advantage (no pointer chase, sequential scan of a packed byte array)
only pays off once the working set is large enough that
`std::unordered_map`'s node has to compete for cache with the surrounding
data. That's why the FlatHashMap lookup numbers get noticeably worse
between `1e5` and `1e7` (8ns → 19ns) — the table has outgrown L2/L3 and
probes start missing cache — while still tracking below or near
`std::unordered_map`'s numbers at every size.

**Probes-per-op stays low and roughly flat** (1.3-6 across all `N`),
consistent with linear probing at a ≤0.75 load factor. `insert`'s probe
count is consistently the highest of the four phases because it's the
only one whose count includes the amortized cost of rehashing (see the
note under "Probe instrumentation" above); `lookup-miss` runs a close
second because a miss can't stop until it hits a genuine `EMPTY` slot,
while a hit or an erase stops as soon as the fingerprint and key match.

**The string workload shows a smaller FlatHashMap margin (~15%)** than
the integer insert/erase numbers. Word-frequency counting is dominated
by hashing and comparing the strings themselves — work both containers
pay equally — rather than by the map's internal bookkeeping, so the
fingerprint's main advantage (skipping most `std::string::operator==`
calls) shows up as a smaller, steadier win instead of the multi-x gap
seen on cheap integer keys where allocator overhead dominates instead.

**Small-`N` insert numbers are noisy** (`N=1e3` shows a higher
FlatHashMap insert time than `N=1e4`) — at that scale, fixed costs
(the first few capacity doublings from 0 → 16 → 32 → ...) and one-shot
effects like cold caches and page faults dominate a timed region that's
only ~1000 operations long, so treat the `1e3` row as noisier than the
rest of the sweep.

## Limitations

- No pointer or reference stability: a rehash moves every live entry
  to a new backing array, invalidating any `V*` returned by an earlier
  `find()`. `std::unordered_map` is mandated by the C++ standard to be
  node-based specifically so that references, pointers, and iterators
  to existing elements stay valid across insertions/erasures (other
  than the erased element itself) — that guarantee is the whole reason
  it cannot be a flat, contiguous table. `FlatHashMap` trades that
  guarantee away for cache-friendly, contiguous storage; this is a
  tradeoff, not a strict improvement over `std::unordered_map`.
- No allocator support.
- No exception safety guarantees.
- Requires `K` and `V` to be default constructible (slots are
  pre-allocated as default-constructed pairs, and `erase()` resets a
  vacated slot to a default-constructed pair).
- No iterators, no `operator[]`, no `emplace`.
- Benchmarks never call `reserve()`/`rehash()` on either container, by
  design — both pay their natural incremental growth cost.
