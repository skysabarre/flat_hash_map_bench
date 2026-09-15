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

_Fill in from `results.csv` / the `bench` console output after
running on your machine. `N` sweeps 1e3, 1e4, 1e5, 1e6, 1e7; each cell
is the median of 4 timed runs (5 run, first discarded as warmup)._

| N | Container | Insert (ns/op) | Lookup-hit (ns/op) | Lookup-miss (ns/op) | Erase (ns/op) | Probes/op (Flat only) |
|---|---|---|---|---|---|---|
| 1e3 | FlatHashMap | | | | | |
| 1e3 | unordered_map | | | | | — |
| 1e4 | FlatHashMap | | | | | |
| 1e4 | unordered_map | | | | | — |
| 1e5 | FlatHashMap | | | | | |
| 1e5 | unordered_map | | | | | — |
| 1e6 | FlatHashMap | | | | | |
| 1e6 | unordered_map | | | | | — |
| 1e7 | FlatHashMap | | | | | |
| 1e7 | unordered_map | | | | | — |

## Results (string keys)

_Word-frequency insert-or-increment loop over the file passed as
`argv[1]`. Fill in after running._

| File | Tokens | Container | Insert-or-increment (ns/op) | Probes/op (Flat only) |
|---|---|---|---|---|
| | | FlatHashMap | | |
| | | unordered_map | | — |

## Analysis

_Fill in after running: where FlatHashMap wins/loses and why (cache
locality vs. rehash/tombstone overhead, string-key comparison cost vs.
fingerprint rejection rate, effect of load factor on probe length,
etc.)._

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
