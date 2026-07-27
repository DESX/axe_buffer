//=============================================================================
// axe_buffer test suite — Catch2 (fetched & built by graft; see Makefile).
// Build:  make            (downloads clang + Catch2, then runs this suite)
//
// Reading order: the first cases are runnable usage examples — start at the top
// to learn the API. The low-level component tests (construction, views) live at
// the bottom under "INTERNALS".
//=============================================================================
#include "catch_amalgamated.hpp"
#include <axe_buffer.hpp> // resolves to the generated, versioned header

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace axe;

//=============================================================================
// USAGE EXAMPLES
//=============================================================================

//----------------------------------------------------------------------------
// EXAMPLE 1 — the whole API at a glance.
//
// axe_buffer<T>(capacity) is a fixed-capacity, single-writer / many-reader
// broadcast ring. The writer appends; once full, appending overwrites the oldest.
// Every reader sees every item it doesn't fall behind on, and readers never
// block the writer.
//----------------------------------------------------------------------------
TEST_CASE("example: producer / consumer at a glance") {
  axe_buffer<int> buf(4); // a ring holding up to 4 ints

  // --- writing ---------------------------------------------------------
  buf.push_back(1); // append a single item
  buf.push_back(2);

  // Append N at once: writer_lock(N) reserves N tail slots and returns an RAII
  // handle. Nothing is visible to readers until the handle commits — which
  // happens automatically when it leaves scope, or eagerly via w.commit().
  {
    auto w = buf.writer_lock(2); // reserve 2 slots
    w[0] = 3;                    // assign by value...
    w[1] = 4;                    // ...or w[i].emplace(args...) in place
  } // <-- committed here, on scope exit

  // snapshot() returns the live window, oldest-to-newest (trivially-copyable
  // T).
  REQUIRE(buf.snapshot() == (std::vector<int>{1, 2, 3, 4}));

  // --- ring overwrite --------------------------------------------------
  buf.push_back(5); // ring is full: oldest (1) is dropped
  buf.push_back(6); // 2 is dropped
  REQUIRE(buf.snapshot() == (std::vector<int>{3, 4, 5, 6}));

  // --- reading ---------------------------------------------------------
  // A reader is a lightweight cursor; read() copies the next item and reports
  // ok / empty / lapped. Create it, then drain until caught up:
  auto r = buf.new_reader(); // starts at the current oldest (3)
  std::vector<int> seen;
  int x;
  while (r.read(x) == read_result::ok)
    seen.push_back(x);
  REQUIRE(seen == (std::vector<int>{3, 4, 5, 6}));
  REQUIRE(r.read(x) == read_result::empty); // nothing new right now

  // If the writer laps a slow reader, the next read() returns `lapped` and the
  // cursor resyncs to the current oldest — the missed items are simply gone:
  for (int v = 7; v <= 20; ++v)
    buf.push_back(v);
  REQUIRE(r.read(x) == read_result::lapped);
  REQUIRE(buf.snapshot() == (std::vector<int>{17, 18, 19, 20}));
}

//----------------------------------------------------------------------------
// EXAMPLE 2 — single-threaded FIFO + overwrite, content checked against a
// std::deque mirror across warmup and many wraps.
//----------------------------------------------------------------------------
TEST_CASE("single-threaded FIFO + overwrite") {
  constexpr size_t S = 4;
  axe_buffer<int> buf(S);
  std::deque<int> mirror;

  auto push = [&](int v) {
    buf.push_back(v);
    mirror.push_back(v);
    while (mirror.size() > S)
      mirror.pop_front();
    std::vector<int> snap = buf.snapshot();
    REQUIRE(snap.size() == mirror.size());
    REQUIRE(std::equal(snap.begin(), snap.end(), mirror.begin()));
  };

  REQUIRE(buf.snapshot().empty());
  for (int v = 10; v <= 25; ++v)
    push(v); // exercises warmup + many overwrites
}

//----------------------------------------------------------------------------
// EXAMPLE 3 — reader cursor: ok / empty / lapped semantics.
//----------------------------------------------------------------------------
TEST_CASE("reader cursor: ok / empty / lapped") {
  constexpr size_t S = 4;
  axe_buffer<int> buf(S);
  for (int v = 0; v < 4; ++v)
    buf.push_back(v); // [0,1,2,3]

  auto r = buf.new_reader();
  int v;
  // Catch2 can't decompose && / ||, so the compound checks get an extra ().
  REQUIRE((r.read(v) == read_result::ok && v == 0));
  REQUIRE((r.read(v) == read_result::ok && v == 1));
  REQUIRE((r.read(v) == read_result::ok && v == 2));
  REQUIRE((r.read(v) == read_result::ok && v == 3));
  REQUIRE(r.read(v) == read_result::empty);

  // advance within capacity: cursor keeps up, no lap
  buf.push_back(4); // [1,2,3,4]
  REQUIRE((r.read(v) == read_result::ok && v == 4));
  REQUIRE(r.read(v) == read_result::empty);

  // overrun the cursor by more than capacity -> lapped, then resynced reads
  for (int x = 5; x <= 10; ++x)
    buf.push_back(x); // [7,8,9,10]
  REQUIRE(r.read(v) == read_result::lapped);
  REQUIRE((r.read(v) == read_result::ok && v == 7));
  REQUIRE((r.read(v) == read_result::ok && v == 8));
  REQUIRE((r.read(v) == read_result::ok && v == 9));
  REQUIRE((r.read(v) == read_result::ok && v == 10));
  REQUIRE(r.read(v) == read_result::empty);
}

//----------------------------------------------------------------------------
// EXAMPLE 3b — temporal tools: remaining_writes() reports an item's headroom in
// writes, and the guarded read read(out, min_margin) declines items too close
// to the overwrite edge with would_overwrite instead of handing them back. The
// library holds no clock: the margin is in writes, and the caller converts its
// own time budget (see DESIGN.md §3).
//----------------------------------------------------------------------------
TEST_CASE("temporal: remaining_writes + guarded read") {
  constexpr size_t S = 8;
  axe_buffer<int> buf(S);
  for (int i = 0; i < 8; ++i)
    buf.push_back(i); // full: freed=0, added=8, so item i sits at depth i

  auto r = buf.new_reader(); // cursor at the oldest (depth 0)
  int x;

  // Oldest item has zero headroom; a request for any positive margin is
  // declined, and declining leaves the cursor exactly where it was.
  REQUIRE(r.remaining_writes() == 0);
  REQUIRE(r.read(x, 1) == read_result::would_overwrite);
  REQUIRE(r.remaining_writes() == 0);
  REQUIRE((r.read(x, 0) == read_result::ok && x == 0)); // margin 0 is satisfiable

  // Walking forward, item i has depth i: margin i is accepted, i+1 declined.
  for (int i = 1; i < 8; ++i) {
    REQUIRE(r.remaining_writes() == i);
    REQUIRE(r.read(x, static_cast<uint64_t>(i) + 1) == read_result::would_overwrite);
    REQUIRE((r.read(x, static_cast<uint64_t>(i)) == read_result::ok && x == i));
  }
  REQUIRE(r.read(x, 0) == read_result::empty); // caught up

  // would_overwrite is an early warning distinct from lapped: as the writer
  // advances, the same item crosses from "too close" to "already gone".
  axe_buffer<int> b2(4);
  for (int i = 0; i < 4; ++i)
    b2.push_back(i);
  auto r2 = b2.new_reader();                              // oldest, depth 0
  REQUIRE(r2.read(x, 2) == read_result::would_overwrite); // 0 < 2: declined, not consumed
  b2.push_back(4);                                        // freed advances past the cursor
  REQUIRE(r2.read(x, 2) == read_result::lapped);          // now genuinely gone; resynced
  REQUIRE(r2.remaining_writes() == 0);                    // cursor sits at the new oldest
}

//----------------------------------------------------------------------------
// EXAMPLE 4 — multi-slot writer_lock(N) + iterator emplace.
//----------------------------------------------------------------------------
TEST_CASE("multi-slot writer_lock + iterator emplace") {
  axe_buffer<int> buf(8);
  {
    auto w = buf.writer_lock(3);
    REQUIRE(w.size() == 3);
    int n = 100;
    for (auto slot : w)
      slot.emplace(n++);
  } // commit on scope exit
  REQUIRE(buf.snapshot() == (std::vector<int>{100, 101, 102}));

  {
    auto w = buf.writer_lock(2);
    w[0] = 200; // proxy operator=
    w[1] = 201;
  } // commit on scope exit
  REQUIRE(buf.snapshot() == (std::vector<int>{100, 101, 102, 200, 201}));
}

//----------------------------------------------------------------------------
// EXAMPLE 5 — writer_range RAII: explicit early commit() publishes and releases
// the mutex; the scope-exit commit is then an idempotent no-op.
//----------------------------------------------------------------------------
TEST_CASE("writer_range explicit early commit") {
  axe_buffer<int> buf(8);
  {
    auto w = buf.writer_lock(2);
    w[0] = 7;
    w[1] = 9;
    w.commit(); // publish before the handle leaves scope
    REQUIRE(buf.snapshot() == (std::vector<int>{7, 9}));
  } // dtor commit() must be a no-op (no double advance)
  REQUIRE(buf.snapshot() == (std::vector<int>{7, 9}));
  // If commit() hadn't released the writer mutex, this second lock would
  // deadlock.
  buf.push_back(11);
  REQUIRE(buf.snapshot() == (std::vector<int>{7, 9, 11}));
}

//----------------------------------------------------------------------------
// EXAMPLE 6 — SPMC under load: 1 writer + N readers. Validates that no torn
// value ever slips through the seqlock and that each reader sees items in FIFO
// order. This is the real concurrent usage pattern.
//----------------------------------------------------------------------------
struct sample {
  uint64_t seq;
  uint64_t chk;
};
static uint64_t mix(uint64_t s) { return (s * 0x9E3779B97F4A7C15ull) ^ (s << 7) ^ 0xA5A5A5ull; }

TEST_CASE("SPMC stress: 1 writer + N readers") {
  constexpr size_t S = 64;
  constexpr uint64_t TOTAL = 500000;
  constexpr int READERS = 4;

  axe_buffer<sample> buf(S);
  std::atomic<bool> done{false};
  std::atomic<uint64_t> total_ok{0};
  std::atomic<int> torn{0};
  std::atomic<int> regress{0};

  auto reader_fn = [&]() {
    auto r = buf.new_reader();
    uint64_t last_seq = 0;
    bool have_last = false;
    uint64_t oks = 0;
    for (;;) {
      sample s;
      read_result rr = r.read(s);
      if (rr == read_result::ok) {
        if (s.chk != mix(s.seq))
          torn.fetch_add(1); // torn snapshot slipped through
        if (have_last && s.seq <= last_seq)
          regress.fetch_add(1); // FIFO order broken
        last_seq = s.seq;
        have_last = true;
        ++oks;
      } else if (rr == read_result::lapped) {
        have_last = false; // forward jump is expected after a lap
      } else               // empty
      {
        if (done.load(std::memory_order_acquire))
          break;
        std::this_thread::yield();
      }
    }
    total_ok.fetch_add(oks);
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < READERS; ++i)
    readers.emplace_back(reader_fn);

  for (uint64_t seq = 0; seq < TOTAL; ++seq)
    buf.push_back(sample{seq, mix(seq)});

  done.store(true, std::memory_order_release);
  for (auto &t : readers)
    t.join();

  REQUIRE(torn.load() == 0);    // seqlock must never admit a torn value
  REQUIRE(regress.load() == 0); // per-reader ok sequence is strictly increasing
  REQUIRE(total_ok.load() > 0); // readers actually observed data
  std::printf("  [stress] total ok reads across %d readers: %llu\n", READERS,
              (unsigned long long)total_ok.load());
}

//----------------------------------------------------------------------------
// EXAMPLE 7 — non-trivial T: emplace/destroy correctness + leak check. For
// non-copyable/non-trivial element types, read via unsafe_window() (validated
// with still_valid()) rather than snapshot().
//----------------------------------------------------------------------------
struct Counted {
  static std::atomic<int> live;
  int v;
  Counted(int x = 0) : v(x) { live.fetch_add(1); }
  Counted(const Counted &o) : v(o.v) { live.fetch_add(1); }
  Counted &operator=(const Counted &o) {
    v = o.v;
    return *this;
  }
  ~Counted() { live.fetch_sub(1); }
};
std::atomic<int> Counted::live{0};

TEST_CASE("non-trivial T: lifetime + leak check") {
  REQUIRE(Counted::live.load() == 0);
  {
    axe_buffer<Counted> buf(4);
    for (int i = 0; i < 20; ++i)
      buf.push_back(i); // many destroy-before-reuse cycles
    // at most S live cells exist at once
    REQUIRE(Counted::live.load() == 4);

    // unsafe reference window (quiescent writer): contents are the last 4
    auto win = buf.unsafe_window();
    std::vector<int> got;
    for (auto seg : win.segs)
      for (auto &c : seg)
        got.push_back(c.v);
    REQUIRE(buf.still_valid(win.word));
    REQUIRE(got == (std::vector<int>{16, 17, 18, 19}));
  }
  REQUIRE(Counted::live.load() == 0); // destructor reclaimed every cell
}

//----------------------------------------------------------------------------
// EXAMPLE 8 — non-trivial T: a single multi-slot lock that straddles the
// warmup->full boundary. was_full == false for this lock, yet some staged slots
// wrap and MUST be destroyed before reuse. Exercises the per-slot reuse
// derivation (was_full || pos >= S) that replaced the cells_constructed_
// counter.
//----------------------------------------------------------------------------
TEST_CASE("non-trivial T: multi-slot lock across warmup->full boundary") {
  REQUIRE(Counted::live.load() == 0);
  {
    axe_buffer<Counted> buf(4);

    // Warmup: stage 3 (no free yet); slots 0,1,2 constructed once each.
    {
      auto w = buf.writer_lock(3);
      int n = 0;
      for (auto s : w)
        s.emplace(n++);
    }
    REQUIRE(Counted::live.load() == 3);

    // One lock for 3 more -> positions 3,4,5. Slot 3 is virgin; slots 0,1 are
    // reused and must be destroyed first. need_free frees positions 0,1.
    {
      auto w = buf.writer_lock(3);
      int n = 10;
      for (auto s : w)
        s.emplace(n++);
    }
    REQUIRE(Counted::live.load() == 4); // exactly S live: no leak, no double-construct

    // Live window is the last 4: positions 2,3,4,5 -> values 2,10,11,12.
    auto win = buf.unsafe_window();
    std::vector<int> got;
    for (auto seg : win.segs)
      for (auto &c : seg)
        got.push_back(c.v);
    REQUIRE(got == (std::vector<int>{2, 10, 11, 12}));
  }
  REQUIRE(Counted::live.load() == 0); // teardown destroyed all live cells
}

//=============================================================================
// TIER-B LIFETIME SAFETY (commit accounting)
//
// commit() publishes only the contiguous prefix of slots actually constructed,
// and teardown destroys the explicit constructed prefix — so neither a partial
// fill nor an exception mid-lock ever publishes or destroys an unwritten cell.
// Both cases assert the invariant that makes this safe: the buffer's live-object
// count is conserved across construction and teardown.
//
// These began as [!shouldfail] demonstrations of the pre-fix defects; the tag is
// gone now that the library upholds the invariant. If either regresses it fails
// loudly rather than silently passing.
//
// Tracer has its own live counter (independent of Counted) and each case
// measures the delta around a scoped buffer, so the cases stay hermetic even
// under randomised test ordering.
//----------------------------------------------------------------------------
struct Tracer {
  static std::atomic<int> live;
  static constexpr int kBoom = 99; // sentinel value whose ctor throws
  int v;
  Tracer(int x = 0) : v(x) {
    if (x == kBoom)
      throw std::runtime_error("Tracer ctor boom");
    live.fetch_add(1);
  }
  Tracer(const Tracer &o) : v(o.v) { live.fetch_add(1); }
  Tracer &operator=(const Tracer &o) {
    v = o.v;
    return *this;
  }
  ~Tracer() { live.fetch_sub(1); }
};
std::atomic<int> Tracer::live{0};

//----------------------------------------------------------------------------
// A throwing element constructor on a fresh slot mid-lock must not corrupt the
// buffer. writer_lock(3) reserves three tail slots; the second emplace throws.
// Stack unwinding runs ~writer_range, which now publishes only the one slot
// actually constructed, and teardown destroys exactly the constructed prefix —
// so the throwing/unwritten slots are never destroyed and the live count is
// conserved. (The throw is on a fresh, non-reused slot; a throw during an
// in-place reuse is a separate Tier-B contract — see DESIGN.md.)
//----------------------------------------------------------------------------
TEST_CASE("throwing ctor mid-lock must not corrupt the buffer") {
  const int before = Tracer::live.load();
  try {
    axe_buffer<Tracer> buf(4);
    buf.push_back(1);
    buf.push_back(2);
    {
      auto w = buf.writer_lock(3); // reserve 3 tail slots
      w[0].emplace(10);
      w[1].emplace(Tracer::kBoom); // throws; only slot 0 (value 10) is committed
      w[2].emplace(12);            // never reached
    }
  } catch (const std::exception &) {
    // expected: the ctor threw and propagated out
  }
  REQUIRE(Tracer::live.load() == before); // nothing unconstructed was destroyed
}

//----------------------------------------------------------------------------
// A partially filled reserved range must not publish unwritten slots.
// writer_lock(3) reserves three slots but only slot 0 is written; commit
// publishes just that prefix, so the unwritten cells never enter the live
// window and teardown never destroys them.
//----------------------------------------------------------------------------
TEST_CASE("partial fill must not publish unwritten slots") {
  const int before = Tracer::live.load();
  {
    axe_buffer<Tracer> buf(8);
    {
      auto w = buf.writer_lock(3); // reserve 3 slots on a fresh buffer
      w[0].emplace(1);             // only slot 0 written; slots 1,2 left unwritten
    } // commit publishes only the constructed prefix

    // The live window is exactly the one written slot — not three.
    auto win = buf.unsafe_window();
    std::vector<int> got;
    for (auto seg : win.segs)
      for (auto &c : seg)
        got.push_back(c.v);
    REQUIRE(got == (std::vector<int>{1}));
  }
  REQUIRE(Tracer::live.load() == before); // teardown destroyed only the one live cell
}

//----------------------------------------------------------------------------
// A throwing element constructor during an in-place *reuse* (overwriting a slot
// in a full ring) must not corrupt the buffer. The reuse path builds the
// replacement in a temporary first, so the throw leaves the existing object
// untouched: no double-destroy, no leak, and the buffer stays usable.
//----------------------------------------------------------------------------
static std::vector<int> window_of(axe_buffer<Tracer> &buf) {
  auto win = buf.unsafe_window();
  std::vector<int> got;
  for (auto seg : win.segs)
    for (auto &c : seg)
      got.push_back(c.v);
  return got;
}

TEST_CASE("throwing ctor during in-place reuse must not corrupt the buffer") {
  const int before = Tracer::live.load();
  {
    axe_buffer<Tracer> buf(4);
    for (int i = 0; i < 4; ++i)
      buf.push_back(i); // fill the ring: live window {0,1,2,3}
    REQUIRE(Tracer::live.load() == before + 4);
    REQUIRE(window_of(buf) == (std::vector<int>{0, 1, 2, 3}));

    // Overwrite two slots (both reuse, ring is full); the second throws.
    try {
      auto w = buf.writer_lock(2);
      w[0].emplace(10);            // reuse: succeeds, evicts oldest (0)
      w[1].emplace(Tracer::kBoom); // reuse: throws while building the temporary
    } catch (const std::exception &) {
      // expected
    }

    // No corruption: exactly S objects are still live. The free is per-cell, so
    // the throwing slot evicted nothing: value 1 survives (only value 0 was
    // dropped, for the one successful overwrite). Window = {1, 2, 3, 10}.
    REQUIRE(Tracer::live.load() == before + 4);
    REQUIRE(window_of(buf) == (std::vector<int>{1, 2, 3, 10}));

    // Buffer is still fully usable afterwards.
    buf.push_back(20);
    REQUIRE(Tracer::live.load() == before + 4);
    REQUIRE(window_of(buf) == (std::vector<int>{2, 3, 10, 20}));
  }
  REQUIRE(Tracer::live.load() == before); // teardown reclaimed every cell
}

//=============================================================================
// CONCURRENCY & EDGE COVERAGE
//=============================================================================

//----------------------------------------------------------------------------
// Multiple writer threads. The mutex serialises writers, so every committed
// item is well-formed: concurrent readers must never see a torn value, and the
// items from any one writer stay in that writer's order (between laps).
//----------------------------------------------------------------------------
TEST_CASE("multi-writer: serialised, no torn reads") {
  constexpr size_t S = 128;
  constexpr int WRITERS = 3;
  constexpr int READERS = 3;
  constexpr uint32_t PER_WRITER = 150000;

  struct tagged {
    uint32_t wid;
    uint32_t seq;
    uint64_t chk;
  };
  auto chk_of = [](uint32_t wid, uint32_t seq) {
    return mix((static_cast<uint64_t>(wid) << 32) | seq);
  };

  axe_buffer<tagged> buf(S);
  std::atomic<bool> done{false};
  std::atomic<int> torn{0}, regress{0};
  std::atomic<uint64_t> total_ok{0};

  auto reader_fn = [&]() {
    auto r = buf.new_reader();
    uint32_t last[WRITERS];
    bool have[WRITERS] = {};
    uint64_t oks = 0;
    for (;;) {
      tagged t;
      read_result rr = r.read(t);
      if (rr == read_result::ok) {
        if (t.chk != chk_of(t.wid, t.seq))
          torn.fetch_add(1);
        else if (t.wid < WRITERS) {
          if (have[t.wid] && t.seq <= last[t.wid])
            regress.fetch_add(1); // a single writer's items must not go backwards
          last[t.wid] = t.seq;
          have[t.wid] = true;
        }
        ++oks;
      } else if (rr == read_result::lapped) {
        for (bool &h : have)
          h = false; // per-writer order only holds within an unlapped run
      } else if (done.load(std::memory_order_acquire)) {
        break;
      } else {
        std::this_thread::yield();
      }
    }
    total_ok.fetch_add(oks);
  };

  std::vector<std::thread> readers, writers;
  for (int i = 0; i < READERS; ++i)
    readers.emplace_back(reader_fn);
  for (int w = 0; w < WRITERS; ++w)
    writers.emplace_back([&, w]() {
      for (uint32_t s = 0; s < PER_WRITER; ++s)
        buf.push_back(tagged{static_cast<uint32_t>(w), s, chk_of(static_cast<uint32_t>(w), s)});
    });
  for (auto &t : writers)
    t.join();
  done.store(true, std::memory_order_release);
  for (auto &t : readers)
    t.join();

  REQUIRE(torn.load() == 0);
  REQUIRE(regress.load() == 0);
  REQUIRE(total_ok.load() > 0);
}

//----------------------------------------------------------------------------
// Degenerate capacities: 1 and 2 must still be correct FIFO/overwrite rings.
//----------------------------------------------------------------------------
TEST_CASE("edge capacities: 1 and 2") {
  for (size_t cap : {size_t{1}, size_t{2}}) {
    axe_buffer<int> buf(cap);
    std::deque<int> mirror;
    for (int v = 0; v < 20; ++v) {
      buf.push_back(v);
      mirror.push_back(v);
      while (mirror.size() > cap)
        mirror.pop_front();
      std::vector<int> snap = buf.snapshot();
      REQUIRE(snap.size() == mirror.size());
      REQUIRE(std::equal(snap.begin(), snap.end(), mirror.begin()));
    }
    auto r = buf.new_reader();
    int x;
    REQUIRE((r.read(x) == read_result::ok && x == 20 - static_cast<int>(cap))); // oldest live
  }
}

//----------------------------------------------------------------------------
// Large, over-aligned T: the heap allocation must honour alignof(T), and the
// per-byte seqlock must never admit a torn value at size under load.
//----------------------------------------------------------------------------
TEST_CASE("large over-aligned T under load") {
  struct alignas(64) wide {
    uint64_t seq;
    uint64_t chk;
    char pad[112]; // sizeof == 128, alignof == 64
  };
  static_assert(sizeof(wide) == 128 && alignof(wide) == 64);

  { // the allocation is over-aligned
    axe_buffer<wide> probe(4);
    wide w{};
    w.seq = 0;
    w.chk = mix(0);
    probe.push_back(w);
    auto win = probe.unsafe_window();
    REQUIRE(win.segs[0].size() == 1);
    REQUIRE(reinterpret_cast<uintptr_t>(win.segs[0].data()) % alignof(wide) == 0);
  }

  constexpr size_t S = 64;
  constexpr uint64_t TOTAL = 50000;
  axe_buffer<wide> buf(S);
  std::atomic<bool> done{false};
  std::atomic<int> torn{0};
  std::atomic<uint64_t> total_ok{0};

  auto reader_fn = [&]() {
    auto r = buf.new_reader();
    wide w;
    uint64_t oks = 0;
    for (;;) {
      read_result rr = r.read(w);
      if (rr == read_result::ok) {
        if (w.chk != mix(w.seq))
          torn.fetch_add(1);
        ++oks;
      } else if (rr == read_result::empty) {
        if (done.load(std::memory_order_acquire))
          break;
        std::this_thread::yield();
      }
      // lapped: resynced, keep going
    }
    total_ok.fetch_add(oks);
  };

  std::vector<std::thread> readers;
  for (int i = 0; i < 2; ++i)
    readers.emplace_back(reader_fn);
  for (uint64_t s = 0; s < TOTAL; ++s) {
    wide w{};
    w.seq = s;
    w.chk = mix(s);
    buf.push_back(w);
  }
  done.store(true, std::memory_order_release);
  for (auto &t : readers)
    t.join();

  REQUIRE(torn.load() == 0);
  REQUIRE(total_ok.load() > 0);
}

//=============================================================================
// INTERNALS — low-level components the ring is built from. Not needed to use
// axe_buffer; here to pin down construction and the view helpers.
//=============================================================================

//----------------------------------------------------------------------------
// runtime capacity: set at construction, reported by capacity(), 0 rejected;
// non-power-of-two sizes wrap and overwrite like any other.
//----------------------------------------------------------------------------
TEST_CASE("runtime capacity") {
  axe_buffer<int> buf(6);
  REQUIRE(buf.capacity() == 6);
  REQUIRE_THROWS_AS(axe_buffer<int>(0), std::invalid_argument);

  axe_buffer<int> odd(3); // not a power of two
  for (int v = 0; v < 7; ++v)
    odd.push_back(v); // 0..6 through a 3-slot ring
  REQUIRE(odd.snapshot() == (std::vector<int>{4, 5, 6}));

  auto r = odd.new_reader();
  int x;
  REQUIRE(r.remaining_writes() == 0); // oldest, depth 0
  REQUIRE((r.read(x) == read_result::ok && x == 4));
  REQUIRE(r.remaining_writes() == 1);
}

//----------------------------------------------------------------------------
// A narrow counter type (uint8_t) wraps its sequence counter every M additions,
// so pushing well past M exercises modular add / distance / index / signed
// distance across the wrap. Both a non-power-of-two capacity (runtime modulus M
// = 255) and a power-of-two capacity (natural 2^8 wrap, M = 0) are checked
// against a std::deque mirror, plus a keeping-up reader that must stay in FIFO
// order across the wrap. Stored values are full ints; only positions wrap.
//----------------------------------------------------------------------------
TEST_CASE("uint8_t counter wraps reliably") {
  auto run = [](size_t cap) {
    axe_buffer<int, uint8_t> buf(cap);
    std::deque<int> mirror;
    auto r = buf.new_reader();
    int got;
    for (int v = 0; v < 900; ++v) { // ~3 full modulus wraps
      buf.push_back(v);
      mirror.push_back(v);
      while (mirror.size() > cap)
        mirror.pop_front();
      std::vector<int> snap = buf.snapshot();
      REQUIRE(snap.size() == mirror.size());
      REQUIRE(std::equal(snap.begin(), snap.end(), mirror.begin()));
      // one push, one read: the reader is never lapped and stays in order.
      REQUIRE((r.read(got) == read_result::ok && got == v));
      REQUIRE(r.read(got) == read_result::empty);
    }
    // Falling behind by more than capacity laps, then resyncs to the oldest.
    // Last value pushed is 901+cap, so the oldest of the final `cap` items is
    // (901+cap) - (cap-1) = 902, independent of capacity.
    for (int v = 900; v <= 901 + static_cast<int>(cap); ++v)
      buf.push_back(v);
    REQUIRE(r.read(got) == read_result::lapped);
    REQUIRE((r.read(got) == read_result::ok && got == 902));
  };

  run(3); // runtime modulus M = 255
  run(8); // natural 2^8 wrap, M = 0
}

//----------------------------------------------------------------------------
// Catch2 owns main() (linked from catch_amalgamated.cpp); a listener is the
// idiomatic hook to surface the version stamped into the generated header.
//----------------------------------------------------------------------------
#ifdef AXE_BUFFER_VERSION
struct version_banner : Catch::EventListenerBase {
  using Catch::EventListenerBase::EventListenerBase;
  void testRunStarting(Catch::TestRunInfo const &) override {
    std::printf("axe_buffer %s\n", AXE_BUFFER_VERSION);
  }
};
CATCH_REGISTER_LISTENER(version_banner)
#endif
