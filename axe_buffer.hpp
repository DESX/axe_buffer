#pragma once
// single-header SPMC (one writer / many readers) FIFO ring.
// Model, packed-word layout, and design notes:
// https://github.com/DESX/axe_buffer

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace axe {
// ---- modular arithmetic ----------------------------------------------------

// Distance from `limit` up to the next multiple of `v` above it.
template <typename T>
constexpr T max_multiple_inv(const T v, const T limit = std::numeric_limits<T>::max()) {
  auto ret = (limit - v + 1) % v;
  return ret == 0 ? v : ret;
}

template <typename T> constexpr bool is_power_of_2(const T val) {
  if (val == 1)
    return true;
  else if ((val == 0) || (val & 1))
    return false;
  else
    return is_power_of_2(static_cast<T>(val >> 1));
}

// Largest multiple of `v` <= `limit`; 0 for v==0 or power-of-two v (caller then
// relies on natural integer overflow).
template <typename T>
constexpr T max_multiple(const T v, const T limit = std::numeric_limits<T>::max()) {
  if (v == 0)
    return 0;
  else if (is_power_of_2(v))
    return 0;
  else
    return limit - max_multiple_inv(v, limit) + 1;
}

// Unsigned arithmetic modulo a compile-time value (0 ⇒ natural integer
// overflow).
template <typename INT_TYPE, INT_TYPE mod_val> class mod_int {
  static_assert(std::is_unsigned_v<INT_TYPE>,
                "mod_int requires an unsigned underlying integer type");

public:
  using this_t = mod_int<INT_TYPE, mod_val>;

  constexpr mod_int() : val_(0) {}

  constexpr mod_int(INT_TYPE val_in) {
    if constexpr (mod_val == 0)
      val_ = val_in;
    else
      val_ = val_in % mod_val;
  }

  constexpr INT_TYPE val() const { return val_; }

  constexpr mod_int operator+(this_t incr) const {
    mod_int ret;
    if constexpr (mod_val == 0) {
      ret.val_ = static_cast<INT_TYPE>(val_ + incr.val_);
    } else {
      INT_TYPE new_val = static_cast<INT_TYPE>(val_ + incr.val_);
      if constexpr (!is_power_of_2(mod_val)) {
        // Correct for INT_TYPE's natural wrap vs mod_val; fires only on
        // overflow.
        if (new_val < val_)
          new_val += max_multiple_inv(mod_val);
      }
      ret.val_ = new_val % mod_val;
    }
    return ret;
  }

  constexpr mod_int operator-(this_t decr) const { return *this + (-decr); }

  constexpr mod_int operator-() const {
    if constexpr (mod_val == 0)
      return mod_int(static_cast<INT_TYPE>(0u - val_));
    else
      return mod_int(static_cast<INT_TYPE>(mod_val - val_));
  }

  constexpr mod_int &operator+=(this_t incr) { return *this = *this + incr; }
  constexpr mod_int &operator-=(this_t decr) { return *this = *this - decr; }

  constexpr mod_int &operator++() { return *this += this_t(1); }
  constexpr mod_int operator++(int) {
    auto tmp = *this;
    ++*this;
    return tmp;
  }

  constexpr bool operator==(const INT_TYPE rhs) const { return val_ == rhs; }
  constexpr bool operator==(const mod_int rhs) const { return val_ == rhs.val_; }
  constexpr bool operator!=(const mod_int rhs) const { return val_ != rhs.val_; }

private:
  INT_TYPE val_;
};

// Signed cyclic distance a→b, in (-mod/2, mod/2].
template <typename INT_TYPE, INT_TYPE mod_val>
constexpr auto signed_distance(mod_int<INT_TYPE, mod_val> a, mod_int<INT_TYPE, mod_val> b)
    -> std::make_signed_t<INT_TYPE> {
  using S = std::make_signed_t<INT_TYPE>;
  if constexpr (mod_val == 0) {
    return static_cast<S>(static_cast<INT_TYPE>(b.val() - a.val()));
  } else {
    const INT_TYPE forward = (b - a).val(); // [0, mod_val)
    if (forward > mod_val / 2)
      return static_cast<S>(forward) - static_cast<S>(mod_val);
    else
      return static_cast<S>(forward);
  }
}

// mod_int over the largest multiple-of-S modulus, plus index() == val() % S.
template <typename T, size_t S>
class mod_index : public mod_int<T, max_multiple<T>(static_cast<T>(S))> {
public:
  using P = mod_int<T, max_multiple<T>(static_cast<T>(S))>;

  static_assert(S > 0, "mod_index size must be > 0");
  static_assert(S <= static_cast<size_t>(std::numeric_limits<T>::max()),
                "mod_index size S exceeds the range of T (use a wider T)");

  constexpr mod_index() : P(0) {}
  constexpr mod_index(T v) : P(v) {}
  constexpr mod_index(P v) : P(v) {}

  using P::val;
  constexpr T index() const { return val() % S; }
};

// Half-open [begin, end) arc over one modulus. The live region and each
// writer's staged span are mod_ranges.
template <typename INT_TYPE, INT_TYPE mod_val> class mod_range {
public:
  using value_t = mod_int<INT_TYPE, mod_val>;

  constexpr mod_range() = default;
  constexpr mod_range(value_t begin, value_t end) : begin_(begin), end_(end) {}

  static constexpr mod_range from_length(value_t begin, INT_TYPE len) {
    return mod_range(begin, begin + value_t(len));
  }

  constexpr value_t begin() const { return begin_; }
  constexpr value_t end() const { return end_; }

  constexpr INT_TYPE size() const { return (end_ - begin_).val(); }
  constexpr bool empty() const { return begin_ == end_; }

  constexpr value_t at(INT_TYPE i) const { return begin_ + value_t(i); }
  constexpr bool contains(value_t p) const { return (p - begin_).val() < size(); }
  constexpr INT_TYPE offset(value_t p) const { return (p - begin_).val(); }

  constexpr mod_range advanced_begin(INT_TYPE n) const {
    return mod_range(begin_ + value_t(n), end_);
  }
  constexpr mod_range advanced_end(INT_TYPE n) const {
    return mod_range(begin_, end_ + value_t(n));
  }

  constexpr bool operator==(const mod_range &o) const {
    return begin_ == o.begin_ && end_ == o.end_;
  }

private:
  value_t begin_{};
  value_t end_{};
};

// ---- cell storage ----------------------------------------------------------
template <typename T> struct naked_block {
  template <typename... Args> T &emplace(Args &&...args) {
    return *(::new (static_cast<void *>(mem)) T(std::forward<Args>(args)...));
  }

  T &ref() { return *std::launder(reinterpret_cast<T *>(mem)); }

  void destroy() { ref().~T(); }

  alignas(T) unsigned char mem[sizeof(T)];
};

// ---- multi-segment views ---------------------------------------------------
template <typename T> class data_view {
public:
  data_view() = default;
  data_view(T *a, size_t s) : begin_(a), end_(a + s) {}
  data_view(T *a, T *b) : begin_(a), end_(b) {}

  size_t size() const { return (end_ && begin_) ? static_cast<size_t>(end_ - begin_) : 0; }
  T *begin() const { return begin_; }
  T *end() const { return end_; }

private:
  T *begin_ = nullptr;
  T *end_ = nullptr;
};

template <typename T, size_t s> class mdata_view {
public:
  size_t size() const {
    size_t sum = 0;
    for (auto &i : data_)
      sum += i.size();
    return sum;
  }
  size_t block_cnt() const { return data_.size(); }

  data_view<T> &operator[](size_t i) { return data_[i]; }
  const data_view<T> &operator[](size_t i) const { return data_[i]; }

  std::vector<std::remove_const_t<T>> to_vec() const {
    std::vector<std::remove_const_t<T>> ret(size());
    auto tmp = ret.begin();
    for (auto &i : data_)
      tmp = std::copy(i.begin(), i.end(), tmp);
    return ret;
  }

private:
  std::array<data_view<T>, s> data_;
};

// ---- axe_buffer<T, S, word_t> ----------------------------------------------
// ok/empty/lapped are the outcomes of an ordinary read. would_overwrite is
// returned only by the guarded read read(out, min_margin): the item exists and
// is not yet lapped, but it has fewer than min_margin writes of headroom before
// the writer could reuse its slot, so the read was declined and the cursor left
// in place. It is distinct from lapped (already gone) — the caller decides
// whether to back off, widen the ring, or accept the item via the plain read().
enum class read_result { ok, empty, lapped, would_overwrite };

template <typename T, size_t S, typename word_t = uint64_t> class axe_buffer {
  static_assert(S > 0, "axe_buffer size must be > 0");
  static_assert(std::is_unsigned_v<word_t>, "counter type must be unsigned");
  static_assert(S <= static_cast<size_t>(std::numeric_limits<word_t>::max()) / 2,
                "S too large for word_t (the counters need a modulus of at least 2S)");

  // Both counters run over the same modulus: the largest multiple of S that
  // fits word_t (0 ⇒ natural overflow, for power-of-two S), so pos % S stays
  // exact.
  static constexpr word_t M = static_cast<word_t>(max_multiple<word_t>(static_cast<word_t>(S)));
  using added_t = mod_int<word_t, M>;
  using range_t = mod_range<word_t, M>; // [freed, added)

  // Writer-side snapshot of the live range — consistent because the writer
  // holds the mutex and is the only mutator.
  range_t live_relaxed() const {
    const added_t f{freed_.load(std::memory_order_relaxed)};
    const added_t a{added_.load(std::memory_order_relaxed)};
    return range_t(f, a);
  }

public:
  using value_type = T;
  static constexpr size_t capacity() { return S; }

  axe_buffer() : added_(0), freed_(0) {}
  ~axe_buffer() {
    // Constructed cells form the prefix [0, constructed_count_): warmup fills
    // 0,1,2,… in order (freed stays 0 until full), and once full every reuse
    // reconstructs in place, so the set never develops a gap. We destroy by the
    // explicit count rather than the live-window size, because a partial or
    // aborted lock can leave freed ahead of what was actually added — the live
    // window then understates how many cells are constructed.
    if constexpr (!std::is_trivially_copyable_v<T>) {
      for (size_t i = 0; i < constructed_count_; ++i)
        cells_[i].destroy();
    }
  }

  axe_buffer(const axe_buffer &) = delete;
  axe_buffer &operator=(const axe_buffer &) = delete;

  // ---- writer side --------------------------------------------------------
  class writer_range;

  // Reserve N tail slots (freeing front space first when full). Returns a
  // move-only RAII handle holding the writer mutex; the slots become visible to
  // readers on commit() or when the handle leaves scope.
  writer_range writer_lock(size_t n) {
    mutex_.lock();
    staged_ = 0; // reset the per-lock construction counter (mutex-protected)
    range_t live = live_relaxed();

    if (n > S)
      n = S; // clamp; N == 0 is a valid no-op

    const size_t sz = live.size();
    const bool was_full = (sz == S);
    const size_t need_free = (sz + n > S) ? (sz + n - S) : 0; // 0 until the ring fills
    if (need_free) {
      // Free-before-write: publish the freed advance (release) BEFORE any cell
      // it frees is overwritten, and fence so the cell writes can't reorder
      // above it — an acquiring reader's post-copy freed load then sees the
      // advance.
      live = live.advanced_begin(static_cast<word_t>(need_free));
      freed_.store(live.begin().val(), std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_release);
    }
    return writer_range(*this, live.end(), n, was_full);
  }

  template <typename... Args> void push_back(Args &&...args) {
    auto r = writer_lock(1);
    r[0].emplace(std::forward<Args>(args)...);
  }

  class slot_proxy {
  public:
    slot_proxy(axe_buffer &b, size_t idx, bool reuse) : buf_(b), idx_(idx), reuse_(reuse) {}

    template <typename... Args> T &emplace(Args &&...args) {
      return buf_.construct_cell(idx_, reuse_, std::forward<Args>(args)...);
    }

    template <typename U> T &operator=(U &&v) { return emplace(std::forward<U>(v)); }

  private:
    axe_buffer &buf_;
    size_t idx_;
    bool reuse_;
  };

  // reuse ⇔ the target cell already holds a constructed object (non-trivial T
  // must be destroyed first): true if the ring was full at lock time, or this
  // is a wrapped position (val ≥ S). Wrap-safe — was_full pins reuse once the
  // ring fills.
  slot_proxy stage_slot(added_t pos, bool was_full) {
    const bool reuse = was_full || (pos.val() >= S);
    return slot_proxy(*this, pos.val() % S, reuse);
  }

  class writer_range {
  public:
    class iterator {
    public:
      iterator(axe_buffer &b, added_t pos, bool was_full)
          : buf_(&b), pos_(pos), was_full_(was_full) {}
      bool operator!=(const iterator &o) const { return pos_ != o.pos_; }
      iterator &operator++() {
        pos_ = pos_ + added_t(1);
        return *this;
      }
      slot_proxy operator*() { return buf_->stage_slot(pos_, was_full_); }

    private:
      axe_buffer *buf_;
      added_t pos_;
      bool was_full_;
    };

    writer_range(axe_buffer &b, added_t start, size_t n, bool was_full)
        : buf_(&b), span_(range_t::from_length(start, static_cast<word_t>(n))),
          was_full_(was_full) {}

    // Move-only: owns the writer mutex and the commit obligation.
    writer_range(writer_range &&o) noexcept
        : buf_(o.buf_), span_(o.span_), was_full_(o.was_full_), committed_(o.committed_) {
      o.buf_ = nullptr;
    }
    writer_range(const writer_range &) = delete;
    writer_range &operator=(const writer_range &) = delete;
    writer_range &operator=(writer_range &&) = delete;
    ~writer_range() { commit(); }

    size_t size() const { return span_.size(); }
    iterator begin() const { return iterator(*buf_, span_.begin(), was_full_); }
    iterator end() const { return iterator(*buf_, span_.end(), was_full_); }
    slot_proxy operator[](size_t i) const {
      return buf_->stage_slot(span_.at(static_cast<word_t>(i)), was_full_);
    }

    // Idempotent; runs on scope exit. Publishes only the contiguous prefix of
    // slots that were actually constructed (staged_), not the full reserved
    // span: a partial fill or an exception mid-lock therefore never exposes an
    // unwritten cell to readers. One release store publishes every staged cell
    // write that precedes it. (staged_ is capped at the span so an accidental
    // over-write past the reservation cannot advance added_ beyond it.)
    void commit() {
      if (!buf_ || committed_)
        return;
      const size_t filled =
          buf_->staged_ < span_.size() ? buf_->staged_ : span_.size();
      const added_t end = span_.begin() + added_t(static_cast<word_t>(filled));
      buf_->added_.store(end.val(), std::memory_order_release);
      committed_ = true;
      buf_->mutex_.unlock();
    }

  private:
    axe_buffer *buf_;
    range_t span_;
    bool was_full_;
    bool committed_ = false;
  };

  // ---- reader side (passive broadcast cursor) -----------------------------
  class reader {
  public:
    explicit reader(axe_buffer &b) : buf_(&b) {
      pos_ = added_t(buf_->freed_.load(std::memory_order_acquire)); // start at oldest
    }

    // ok: out written, cursor advanced. empty: caught up. lapped: freed crossed
    // the cursor during the copy (the item is gone) — cursor resyncs to oldest,
    // out untouched. Trivially-copyable T only.
    //
    // No retry loop: a concurrent commit can't tear the copy (only freed gates
    // the cell's reuse), and a free past pos is a genuine lap, which a retry
    // could never recover. freed is monotonic, so the single post-copy check
    // that freed <= pos proves freed <= pos for the whole copy ⇒ the bytes are
    // clean.
    read_result read(T &out) {
      static_assert(std::is_trivially_copyable_v<T>,
                    "reader::read requires trivially-copyable T; use "
                    "unsafe_window otherwise");
      // acquire makes pos's committed cell write visible before the copy.
      const added_t added{buf_->added_.load(std::memory_order_acquire)};
      if (pos_ == added)
        return read_result::empty;

      const size_t idx = pos_.val() % S;
      atomic_load_bytes(&out, buf_->cells_[idx].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire); // copy before the freed load
      const added_t freed{buf_->freed_.load(std::memory_order_relaxed)};

      if (signed_distance(freed, pos_) < 0) {
        pos_ = freed;
        return read_result::lapped;
      }
      pos_ = pos_ + added_t(1);
      return read_result::ok;
    }

    // Headroom, in writes, before the item at the cursor (the next one read()
    // would return) could be overwritten: signed_distance(freed, cursor). It is
    // the item's depth above the reclaim edge — 0 for the oldest live item,
    // up to S-1 for the newest, negative once lapped. This is a snapshot that
    // can only shrink as the writer advances; the library holds no clock, so a
    // caller with a bounded read/processing time and a max write rate converts
    // its own time budget into a write-count margin (see DESIGN.md §3). Works
    // for any T (it inspects only the counters, never the cells).
    std::make_signed_t<word_t> remaining_writes() const {
      const added_t freed{buf_->freed_.load(std::memory_order_acquire)};
      return signed_distance(freed, pos_);
    }

    // Guarded read: like read(out), but declines with would_overwrite (leaving
    // the cursor put) when the cursor's item has fewer than min_margin writes of
    // headroom. An already-lapped cursor still reports lapped and resyncs. The
    // seqlock still guarantees the copy is untorn, so min_margin is purely the
    // caller's temporal policy — it never affects correctness for trivial T,
    // only which items are handed back. Trivially-copyable T only.
    read_result read(T &out, word_t min_margin) {
      static_assert(std::is_trivially_copyable_v<T>,
                    "reader::read requires trivially-copyable T; use "
                    "unsafe_window otherwise");
      const added_t added{buf_->added_.load(std::memory_order_acquire)};
      if (pos_ == added)
        return read_result::empty;

      const added_t freed{buf_->freed_.load(std::memory_order_acquire)};
      const auto margin = signed_distance(freed, pos_);
      if (margin < 0) { // already reclaimed
        pos_ = freed;
        return read_result::lapped;
      }
      if (static_cast<word_t>(margin) < min_margin)
        return read_result::would_overwrite; // exists, but too close to the edge

      const size_t idx = pos_.val() % S;
      atomic_load_bytes(&out, buf_->cells_[idx].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      const added_t freed2{buf_->freed_.load(std::memory_order_relaxed)};
      if (signed_distance(freed2, pos_) < 0) {
        pos_ = freed2;
        return read_result::lapped;
      }
      pos_ = pos_ + added_t(1);
      return read_result::ok;
    }

  private:
    axe_buffer *buf_;
    added_t pos_{};
  };

  reader new_reader() { return reader(*this); }

  // Consistent oldest-to-newest copy of the live window. Trivially-copyable T
  // only.
  std::vector<T> snapshot() const {
    static_assert(std::is_trivially_copyable_v<T>,
                  "snapshot requires trivially-copyable T; use unsafe_window otherwise");
    for (;;) {
      const word_t f1 = freed_.load(std::memory_order_acquire);    // read freed first
      const added_t added{added_.load(std::memory_order_acquire)}; // ⇒ added >= f1
      const range_t live(added_t(f1), added);
      std::vector<T> out(live.size());
      for (word_t k = 0; k < live.size(); ++k)
        atomic_load_bytes(&out[k], cells_[live.at(k).val() % S].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire); // copy before the freed recheck
      // freed unchanged ⇒ no copied cell was freed/overwritten during the copy.
      if (freed_.load(std::memory_order_relaxed) == f1)
        return out;
    }
  }

  // Up to two contiguous reference-segments over the live region, plus the
  // freed counter it was observed under. UNSAFE for non-trivial T: references
  // stay valid only while the writer cannot lap the reader (quiescent writer,
  // or sized so the window can't advance by S during use). Call
  // still_valid(word) AFTER reading to detect (not prevent) a lap and discard
  // the result; reading a cell mid-reconstruction is UB.
  struct unsafe_view {
    mdata_view<T, 2> segs;
    word_t word;
  };

  unsafe_view unsafe_window() {
    const word_t f = freed_.load(std::memory_order_acquire);
    const added_t added{added_.load(std::memory_order_acquire)};
    const range_t live(added_t(f), added);
    unsafe_view uv;
    uv.word = f; // lap sentinel: a lap can only advance freed
    // naked_block<T> is just aligned T-storage, so cells address like a plain
    // T[].
    T *const base = reinterpret_cast<T *>(cells_.data());
    const size_t a = live.begin().val() % S;
    const size_t b = live.end().val() % S;
    if (live.empty()) {
    } else if (a < b) {
      uv.segs[0] = {base + a, base + b};
    } else {
      uv.segs[0] = {base + a, base + S};
      uv.segs[1] = {base + 0, base + b};
    }
    return uv;
  }

  bool still_valid(word_t observed) const {
    return freed_.load(std::memory_order_acquire) == observed;
  }

private:
  // Per-byte relaxed atomics make concurrent reader/writer cell access
  // race-free; the packed word's acquire/release plus the reader's acquire
  // fence give ordering.
  static void atomic_store_bytes(void *dst, const void *src, size_t n) {
    auto *d = static_cast<unsigned char *>(dst);
    auto *s = static_cast<const unsigned char *>(src);
    for (size_t i = 0; i < n; ++i)
      std::atomic_ref<unsigned char>(d[i]).store(s[i], std::memory_order_relaxed);
  }
  static void atomic_load_bytes(void *dst, const void *src, size_t n) {
    auto *d = static_cast<unsigned char *>(dst);
    auto *s = const_cast<unsigned char *>(static_cast<const unsigned char *>(src));
    for (size_t i = 0; i < n; ++i)
      d[i] = std::atomic_ref<unsigned char>(s[i]).load(std::memory_order_relaxed);
  }

  // Trivially-copyable T: race-free byte store (trivial dtor, nothing to
  // destroy). Non-trivial T: placement-new, destroying the prior object first
  // when reused.
  template <typename... Args> T &construct_cell(size_t idx, bool reuse, Args &&...args) {
    if constexpr (std::is_trivially_copyable_v<T>) {
      (void)reuse;
      T tmp(std::forward<Args>(args)...); // may throw before anything is published
      atomic_store_bytes(cells_[idx].mem, &tmp, sizeof(T));
      ++staged_; // count only after a successful store
      return cells_[idx].ref();
    } else {
      static_assert(std::is_move_constructible_v<T> ||
                        std::is_copy_constructible_v<T>,
                    "non-trivial axe_buffer element must be move- or "
                    "copy-constructible: the ring overwrites cells on wrap");
      if (!reuse) {
        // Fresh slot: nothing to destroy, so a throw is already safe — neither
        // counter has advanced and no existing object was touched.
        T &ref = cells_[idx].emplace(std::forward<Args>(args)...);
        ++constructed_count_; // grows over [0, S) in warmup, then pinned at S
        ++staged_;
        return ref;
      }
      // Reuse overwrites in place, so the old object must be destroyed before
      // the new one occupies the same address. Build the replacement in a
      // temporary FIRST: a throwing element constructor then leaves the existing
      // object untouched (strong guarantee — no half-overwritten hole for
      // teardown or a later reuse to trip over). Only once the temporary exists
      // do we destroy and move it in; move_if_noexcept keeps that final step
      // non-throwing whenever T has a noexcept move or a copy — i.e. for
      // effectively every broadcastable type.
      T tmp(std::forward<Args>(args)...); // throw => cell unchanged, not counted
      cells_[idx].destroy();
      T &ref = cells_[idx].emplace(std::move_if_noexcept(tmp));
      ++staged_;
      return ref;
    }
  }

  std::array<naked_block<T>, S> cells_{};
  std::atomic<word_t> added_;    // committed sequence count (mod M)
  std::atomic<word_t> freed_;    // freed sequence count     (mod M)
  std::mutex mutex_;             // serialises writers
  size_t constructed_count_ = 0; // # constructed cells = prefix [0, count); teardown only
  size_t staged_ = 0;            // successful constructs in the current lock; mutex-protected
};

} // namespace axe
