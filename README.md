# axe_buffer

Single-header C++20 single-producer, multi-consumer (SPMC) broadcast ring. One
writer appends fixed-capacity FIFO items; many readers each observe the full
stream independently. When the ring fills, new items overwrite the oldest.
Readers are wait-free and passive: they never block the writer and never mutate
shared state beyond their own cursor. Torn reads are detected rather than
prevented, via a seqlock over two monotonic counters.

Requires C++20 (`std::atomic_ref`, `std::move_if_noexcept`). No dependencies
beyond the standard library.

## Features

* [Broadcast SPMC ring](#broadcast-spmc-ring): fixed capacity, one writer, many independent readers
* [Wait-free passive readers](#wait-free-passive-readers): readers hold only a cursor, never block the writer
* [Overwrite with lap detection](#overwrite-with-lap-detection): a full ring drops the oldest; slow readers get `lapped`
* [Batched writes](#batched-writes): reserve N slots, fill them, commit atomically on scope exit
* [Consistent snapshots](#consistent-snapshots): coherent oldest-to-newest copy of the live window
* [Non-trivial element types (Tier B)](#non-trivial-element-types-tier-b): store any type, read it by reference
* [Temporal safety tools](#temporal-safety-tools): decline reads too close to the overwrite edge
* [Counter core and configuration](#counter-core-and-configuration): wrap-safe counters, configurable word width

## Usage

```cpp
#include <axe_buffer.hpp>
using namespace axe;

axe_buffer<int> buf(4);   // ring holding up to 4 ints; capacity is a runtime arg
```

Append single items. Once the ring is full, each append drops the oldest item.

```cpp
buf.push_back(1);
buf.push_back(2);
// buf.snapshot() == {1, 2}
```

Append several items in one transaction. `writer_lock(n)` reserves `n` tail slots
and returns a move-only RAII handle that holds the writer mutex. Nothing is
visible to readers until the handle commits, which happens on scope exit or via
an explicit `commit()`.

```cpp
{
  auto w = buf.writer_lock(2);
  w[0] = 3;              // assign by value
  w[1].emplace(4);       // or construct in place
}                        // committed here
// buf.snapshot() == {1, 2, 3, 4}
```

Overwrite the oldest once full.

```cpp
buf.push_back(5);        // 1 dropped
buf.push_back(6);        // 2 dropped
// buf.snapshot() == {3, 4, 5, 6}
```

Read through a cursor. A reader is a lightweight, independent view of the stream.
`read()` copies the next item and reports the outcome.

```cpp
auto r = buf.new_reader();   // starts at the current oldest
int x;
while (r.read(x) == read_result::ok) {
  // use x
}
// read() returned empty: the cursor is caught up
```

If the writer laps a slow reader, the next `read()` returns `lapped` and the
cursor resyncs to the current oldest. The missed items are gone.

```cpp
for (int v = 7; v <= 20; ++v) buf.push_back(v);
r.read(x);   // read_result::lapped
```

Decline items that are too close to the overwrite edge. `read(out, min_margin)`
returns `would_overwrite` unless the item has at least `min_margin` writes of
headroom left.

```cpp
if (r.read(x, 8) == read_result::would_overwrite) {
  // fewer than 8 writes of headroom; back off or accept via read(x)
}
```

Store non-trivial types and read them by reference.

```cpp
axe_buffer<std::string> log(16);
log.push_back("hello");

auto win = log.unsafe_window();          // up to two contiguous std::span<T>, no copy
for (auto seg : win.segs)
  for (std::string& s : seg)
    ; // use s
if (!log.still_valid(win.word)) {
  // writer lapped the region during use; discard the result
}
```

## Broadcast SPMC ring

`axe_buffer<T>` is a fixed-capacity ring; capacity is a constructor argument
(`axe_buffer<T> buf(n)`), so it can come from a config file rather than a
template parameter. A single writer appends; the mutex serializes writers, so
multiple writer threads are allowed but only one is active at a time. Every
reader receives every item it does not fall behind on. Readers are independent:
consuming from one cursor does not affect any other.

State is two monotonic 64-bit counters, `added_` (committed) and `freed_`
(reclaimed). The live window is the half-open range `[freed_, added_)`.
`capacity()` returns the size; `value_type` is `T`. A capacity of 0 throws
`std::invalid_argument`.

## Wait-free passive readers

A reader holds only a thread-local cursor. It writes no shared state, so the
writer cannot observe that a reader exists, and no reader can stall the writer or
another reader. This is stronger than lock-free: readers are invisible.

The cost of invisibility is that reader safety is detection, not prevention (see
[Overwrite with lap detection](#overwrite-with-lap-detection)). It also means the
writer cannot know when a cell is unreferenced, which is why in-place reclamation
of [non-trivial types](#non-trivial-element-types-tier-b) needs extra care.

## Overwrite with lap detection

For trivially-copyable `T`, `read()` is a seqlock. The reader copies the cell
with per-byte relaxed atomics (data-race free), then loads `freed_` behind an
acquire fence. Because `freed_` is monotonic and the writer publishes a free
before it overwrites the freed cell, a single post-copy check proves whether the
bytes were stable for the whole copy. If `freed_` crossed the cursor, the copy is
discarded and `read()` returns `lapped`; the cursor resyncs to the current
oldest.

`read_result` values:

* `ok`: `out` written, cursor advanced.
* `empty`: caught up, nothing new.
* `lapped`: the item was overwritten; cursor resynced, `out` untouched.
* `would_overwrite`: only from the [guarded read](#temporal-safety-tools).

## Batched writes

`writer_lock(n)` reserves `n` tail slots and returns a `writer_range`. The handle
is move-only and owns both the writer mutex and the commit obligation. Fill slots
via `operator[]`, the range iterator, or `slot_proxy::emplace`. Slots become
visible atomically at commit: one release store to `added_` publishes every cell
write that precedes it.

Commit runs on scope exit, or eagerly via `commit()` (idempotent; a later
scope-exit commit is a no-op). Commit publishes only the contiguous prefix of
slots actually constructed, so a partial fill or an exception mid-lock never
exposes an unwritten cell. `push_back` is `writer_lock(1)` plus one emplace.

## Consistent snapshots

`snapshot()` returns a `std::vector<T>` copy of the live window, oldest to
newest, taken under the same seqlock discipline as `read()`. It retries if the
writer reclaims any copied cell during the pass, so the result is a coherent
point-in-time view. Trivially-copyable `T` only.

## Non-trivial element types (Tier B)

Non-trivial `T` (types with a nontrivial copy or destructor) is supported for
storage and single-consumer or quiescent-writer reads. `read()` and `snapshot()`
are unavailable for such types, because a copy constructor run over a
concurrently-mutating cell is undefined behavior that no post-hoc check can
undo.

Read non-trivial elements with `unsafe_window()`, which returns up to two
contiguous reference segments over the live region plus the `freed_` value it was
observed under. References stay valid only while the writer cannot lap the region:
a quiescent writer, or a ring sized and paced so the window cannot advance by `S`
during use (see [Temporal safety tools](#temporal-safety-tools)). Call
`still_valid(word)` after use to detect a lap and discard the result. Reading a
cell mid-reconstruction is undefined behavior; this detects a lap, it does not
prevent one.

Overwrite is exception-safe. A reused slot builds its replacement in a temporary
first, so a throwing element constructor leaves the existing object intact, then
places the temporary with `std::move_if_noexcept`. Non-trivial `T` must be move-
or copy-constructible.

## Temporal safety tools

The library holds no clock and measures no rate. Margins are counted in writes;
the caller converts its own read-time budget and write-rate bound into a
write-count margin.

* `reader::remaining_writes()` returns the headroom, in writes, before the
  cursor's item is reclaimed: `0` for the oldest live item, up to `capacity-1` for the
  newest, negative once lapped. It inspects only the counters, so it is available
  for any `T`.
* `reader::read(out, min_margin)` is a guarded read. It returns `ok` only when
  `remaining_writes() >= min_margin`, and `would_overwrite` when the item exists
  but has too little headroom (the cursor is left in place). For trivially-
  copyable `T` the seqlock still guarantees an untorn copy, so `min_margin` is
  pure policy: it selects which items are handed back, never correctness.

`would_overwrite` is distinct from `lapped`. It is an early warning that an item
is about to be overwritten, not a report that it already was.

## Counter core and configuration

Capacity is a runtime constructor argument of any positive value; it need not be
a power of two. Cells live in one heap allocation (`unique_ptr<...[]>`) sized at
construction and never resized, so the buffer is a single-process, multi-thread
object (not placed in shared memory).

The two sequence counters are `uint64_t` and increment naturally, wrapping at
`2^64`. A cell index is `pos % capacity`. Because the counters never wrap in
practice (`2^64` writes is centuries away at any real rate), that modulo is exact
for every capacity. Runtime capacity means the modulo is a real hardware divide;
a power-of-two capacity is not required but lets some compilers and the reader
avoid the division. Lap detection uses the signed difference of two counters and
is correct while positions stay within `2^63` of each other, which the live
window (at most `capacity`) always satisfies.

## Build

`make` fetches a pinned Clang and Catch2 (via graft), stamps a version into the
distributable header, then builds and runs the test suite. Other targets:
`make header` (generate the versioned header only), `make lint`, `make format`.

See `DESIGN.md` for the full concurrency model, the safety tiers, and the
temporal-margin analysis.
