//=============================================================================
// axe_buffer test suite — Catch2 (fetched & built by graft; see Makefile).
// Build:  make            (downloads clang + Catch2, then runs this suite)
//
// Reading order: the first cases are runnable usage examples — start at the top
// to learn the API. The low-level component tests (modular arithmetic, ranges,
// views) live at the bottom under "INTERNALS".
//=============================================================================
#include "catch_amalgamated.hpp"
#include <axe_buffer.hpp> // resolves to the generated, versioned header

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <thread>
#include <vector>

using namespace axe;

// mod_range exposes begin()/end(), so Catch2 mistakes it for an iterable range
// and tries to print it by dereferencing mod_int (which isn't an iterator).
// Opt it out of range-stringification; it falls back to "{?}" on failure.
namespace Catch {
template <typename W, W M> struct is_range<axe::mod_range<W, M>> : std::false_type {};
} // namespace Catch

//=============================================================================
// USAGE EXAMPLES
//=============================================================================

//----------------------------------------------------------------------------
// EXAMPLE 1 — the whole API at a glance.
//
// axe_buffer<T, S> is a fixed-capacity, single-writer / many-reader broadcast
// ring. The writer appends; once full, appending overwrites the oldest item.
// Every reader sees every item it doesn't fall behind on, and readers never
// block the writer.
//----------------------------------------------------------------------------
TEST_CASE("example: producer / consumer at a glance") {
  axe_buffer<int, 4> buf; // a ring holding up to 4 ints

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
  axe_buffer<int, S> buf;
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
  axe_buffer<int, S> buf;
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
// EXAMPLE 4 — multi-slot writer_lock(N) + iterator emplace.
//----------------------------------------------------------------------------
TEST_CASE("multi-slot writer_lock + iterator emplace") {
  axe_buffer<int, 8> buf;
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
  axe_buffer<int, 8> buf;
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

  axe_buffer<sample, S> buf;
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
    axe_buffer<Counted, 4> buf;
    for (int i = 0; i < 20; ++i)
      buf.push_back(i); // many destroy-before-reuse cycles
    // at most S live cells exist at once
    REQUIRE(Counted::live.load() == 4);

    // unsafe reference window (quiescent writer): contents are the last 4
    auto win = buf.unsafe_window();
    std::vector<int> got;
    for (size_t seg = 0; seg < win.segs.block_cnt(); ++seg)
      for (Counted *p = win.segs[seg].begin(); p != win.segs[seg].end(); ++p)
        got.push_back(p->v);
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
    axe_buffer<Counted, 4> buf;

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
    for (size_t seg = 0; seg < win.segs.block_cnt(); ++seg)
      for (Counted *p = win.segs[seg].begin(); p != win.segs[seg].end(); ++p)
        got.push_back(p->v);
    REQUIRE(got == (std::vector<int>{2, 10, 11, 12}));
  }
  REQUIRE(Counted::live.load() == 0); // teardown destroyed all live cells
}

//=============================================================================
// INTERNALS — low-level components the ring is built from. Not needed to use
// axe_buffer; here to pin down the modular-arithmetic edge cases.
//=============================================================================

//----------------------------------------------------------------------------
// modulo arithmetic core
//----------------------------------------------------------------------------
TEST_CASE("modulo arithmetic core") {
  REQUIRE(is_power_of_2<uint8_t>(1));
  REQUIRE(is_power_of_2<uint8_t>(128));
  REQUIRE(!is_power_of_2<uint8_t>(0));
  REQUIRE(!is_power_of_2<uint8_t>(6));
  REQUIRE(!is_power_of_2<uint8_t>(255));

  REQUIRE(max_multiple<uint8_t>(1) == 0);
  REQUIRE(max_multiple<uint8_t>(2) == 0); // power of 2 -> natural overflow
  REQUIRE(max_multiple<uint8_t>(3) == 255);
  REQUIRE(max_multiple<uint8_t>(10) == 250);
  REQUIRE(max_multiple<uint8_t>(100) == 200);

  mod_int<uint8_t, 10> v = 4;
  REQUIRE((++v) == 5);
  REQUIRE((v++) == 5);
  REQUIRE(v == 6);
  v += 2;
  REQUIRE(v == 8);
  v += 2;
  REQUIRE(v == 0);
  v -= 1;
  REQUIRE(v.val() == 9);

  // wrap-aware addition on non-power-of-2 modulus
  mod_int<uint8_t, 10> r = 9;
  r += 255;
  REQUIRE(r.val() == 4);

  mod_index<uint8_t, 5> idx = (uint8_t)22;
  REQUIRE(idx.val() == 22);
  REQUIRE(idx.index() == 2);
  ++idx;
  REQUIRE(idx.index() == 3);

  // signed_distance — modular and natural-overflow
  using mi = mod_int<uint32_t, 1000>;
  REQUIRE(signed_distance(mi(100), mi(150)) == 50);
  REQUIRE(signed_distance(mi(150), mi(100)) == -50);
  REQUIRE(signed_distance(mi(990), mi(10)) == 20); // across wrap
  using nat = mod_int<uint32_t, 0>;
  REQUIRE(signed_distance(nat(0xFFFFFFFEu), nat(2u)) == 4);
}

//----------------------------------------------------------------------------
// mod_range — pair of mod_ints as a half-open buffer range
//----------------------------------------------------------------------------
TEST_CASE("mod_range half-open buffer range") {
  using rng = mod_range<uint32_t, 1000>;
  using mi = mod_int<uint32_t, 1000>;

  rng r(mi(10), mi(15)); // [10, 15)
  REQUIRE(r.size() == 5);
  REQUIRE(!r.empty());
  REQUIRE(r.begin() == mi(10));
  REQUIRE(r.end() == mi(15));
  REQUIRE(r.contains(mi(10)));
  REQUIRE(r.contains(mi(14)));
  REQUIRE(!r.contains(mi(15))); // end is exclusive
  REQUIRE(!r.contains(mi(9)));
  REQUIRE(r.offset(mi(12)) == 2);
  REQUIRE(r.at(3) == mi(13));

  // empty range
  REQUIRE(rng(mi(7), mi(7)).empty());
  REQUIRE(rng(mi(7), mi(7)).size() == 0);

  // from_length and the advance helpers (freeing front / committing back)
  rng f = rng::from_length(mi(998), 5); // [998, 3) across the wrap
  REQUIRE(f.size() == 5);
  REQUIRE(f.contains(mi(999)));
  REQUIRE(f.contains(mi(0)));
  REQUIRE(f.contains(mi(2)));
  REQUIRE(!f.contains(mi(3)));
  REQUIRE(f.offset(mi(0)) == 2);
  REQUIRE(f.advanced_begin(2) == rng(mi(0), mi(3))); // freed 2 from front
  REQUIRE(f.advanced_end(2) == rng(mi(998), mi(5))); // committed 2 at back

  // natural-overflow modulus (mod_val == 0) wraps at 2^32
  using rng0 = mod_range<uint32_t, 0>;
  using mi0 = mod_int<uint32_t, 0>;
  rng0 w = rng0::from_length(mi0(0xFFFFFFFEu), 4); // [..FE, 2)
  REQUIRE(w.size() == 4);
  REQUIRE(w.contains(mi0(0xFFFFFFFFu)));
  REQUIRE(w.contains(mi0(1u)));
  REQUIRE(!w.contains(mi0(2u)));
}

//----------------------------------------------------------------------------
// multi-segment view
//----------------------------------------------------------------------------
TEST_CASE("multi-segment view") {
  mdata_view<char, 2> d;
  char a[5] = {'a', 'b', 'c', 'd', 'e'};
  char b[3] = {'f', 'g', 'h'};
  d[0] = {a, sizeof(a)};
  d[1] = {b, sizeof(b)};
  REQUIRE(d.size() == 8);
  REQUIRE(d.to_vec() == (std::vector<char>{'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'}));
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
