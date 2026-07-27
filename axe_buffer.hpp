#pragma once
// single-header SPMC (one writer / many readers) FIFO ring.
// Runtime-sized: capacity is a constructor argument.
// Model and design notes: https://github.com/DESX/axe_buffer

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace axe {

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

// ---- axe_buffer<T> ---------------------------------------------------------
// ok/empty/lapped are the outcomes of an ordinary read. would_overwrite is
// returned only by the guarded read read(out, min_margin): the item exists and
// is not yet lapped, but it has fewer than min_margin writes of headroom before
// the writer could reuse its slot, so the read was declined and the cursor left
// in place. It is distinct from lapped (already gone): the caller decides
// whether to back off, widen the ring, or accept the item via the plain read().
enum class read_result { ok, empty, lapped, would_overwrite };

template <typename T> class axe_buffer {
  // Monotonic sequence counter. Wraps naturally at 2^64; at any realistic write
  // rate that boundary is centuries away, so pos % capacity is exact in
  // practice for every capacity (power-of-two or not).
  using pos_t = uint64_t;

  static size_t validate_capacity(size_t c) {
    if (c == 0)
      throw std::invalid_argument("axe_buffer capacity must be > 0");
    return c;
  }

  // Cell index for a sequence position. Runtime capacity means this is a real
  // divide (a compile-time size would strength-reduce to a multiply-shift).
  size_t index(pos_t p) const { return static_cast<size_t>(p % cap_); }

  // Signed cyclic distance a->b. Correct while |b - a| < 2^63, which holds: the
  // live window is at most `capacity` wide and only nearby positions are ever
  // compared.
  static int64_t signed_distance(pos_t a, pos_t b) {
    return static_cast<int64_t>(b - a);
  }

  // Half-open [begin, end) span of sequence positions. The live region and each
  // writer's staged span are ranges. Plain arithmetic: no modulus here, indexing
  // happens via index() at the point of cell access.
  struct range {
    pos_t begin_ = 0;
    pos_t end_ = 0;
    static range from_length(pos_t begin, uint64_t len) { return {begin, begin + len}; }
    pos_t begin() const { return begin_; }
    pos_t end() const { return end_; }
    uint64_t size() const { return end_ - begin_; }
    bool empty() const { return begin_ == end_; }
    pos_t at(uint64_t i) const { return begin_ + i; }
    range advanced_begin(uint64_t n) const { return {begin_ + n, end_}; }
  };

  // Writer-side snapshot of the live range, consistent because the writer holds
  // the mutex and is the only mutator.
  range live_relaxed() const {
    return range{freed_.load(std::memory_order_relaxed),
                 added_.load(std::memory_order_relaxed)};
  }

public:
  using value_type = T;
  size_t capacity() const { return cap_; }

  explicit axe_buffer(size_t capacity)
      : cap_(validate_capacity(capacity)),
        cells_(std::make_unique<naked_block<T>[]>(cap_)), added_(0), freed_(0) {}

  ~axe_buffer() {
    // Constructed cells form the prefix [0, constructed_count_): warmup fills
    // 0,1,2,… in order (freed stays 0 until full), and once full every reuse
    // reconstructs in place, so the set never develops a gap. We destroy by the
    // explicit count rather than the live-window size, because a partial or
    // aborted lock can leave freed ahead of what was actually added: the live
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
    range live = live_relaxed();

    if (n > cap_)
      n = cap_; // clamp; N == 0 is a valid no-op

    const size_t sz = static_cast<size_t>(live.size());
    const bool was_full = (sz == cap_);
    const size_t need_free = (sz + n > cap_) ? (sz + n - cap_) : 0; // 0 until full
    if (need_free) {
      // Free-before-write: publish the freed advance (release) BEFORE any cell
      // it frees is overwritten, and fence so the cell writes can't reorder
      // above it: an acquiring reader's post-copy freed load then sees the
      // advance.
      live = live.advanced_begin(need_free);
      freed_.store(live.begin(), std::memory_order_release);
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
  // must be destroyed first): true if the ring was full at lock time, or this is
  // a wrapped position (pos ≥ capacity). Wrap-safe: was_full pins reuse once the
  // ring fills.
  slot_proxy stage_slot(pos_t pos, bool was_full) {
    const bool reuse = was_full || (pos >= cap_);
    return slot_proxy(*this, index(pos), reuse);
  }

  class writer_range {
  public:
    class iterator {
    public:
      iterator(axe_buffer &b, pos_t pos, bool was_full)
          : buf_(&b), pos_(pos), was_full_(was_full) {}
      bool operator!=(const iterator &o) const { return pos_ != o.pos_; }
      iterator &operator++() {
        ++pos_;
        return *this;
      }
      slot_proxy operator*() { return buf_->stage_slot(pos_, was_full_); }

    private:
      axe_buffer *buf_;
      pos_t pos_;
      bool was_full_;
    };

    writer_range(axe_buffer &b, pos_t start, size_t n, bool was_full)
        : buf_(&b), span_(range::from_length(start, n)), was_full_(was_full) {}

    // Move-only: owns the writer mutex and the commit obligation.
    writer_range(writer_range &&o) noexcept
        : buf_(o.buf_), span_(o.span_), was_full_(o.was_full_), committed_(o.committed_) {
      o.buf_ = nullptr;
    }
    writer_range(const writer_range &) = delete;
    writer_range &operator=(const writer_range &) = delete;
    writer_range &operator=(writer_range &&) = delete;
    ~writer_range() { commit(); }

    size_t size() const { return static_cast<size_t>(span_.size()); }
    iterator begin() const { return iterator(*buf_, span_.begin(), was_full_); }
    iterator end() const { return iterator(*buf_, span_.end(), was_full_); }
    slot_proxy operator[](size_t i) const {
      return buf_->stage_slot(span_.at(i), was_full_);
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
      const uint64_t span = span_.size();
      const uint64_t filled = buf_->staged_ < span ? buf_->staged_ : span;
      buf_->added_.store(span_.begin() + filled, std::memory_order_release);
      committed_ = true;
      buf_->mutex_.unlock();
    }

  private:
    axe_buffer *buf_;
    range span_;
    bool was_full_;
    bool committed_ = false;
  };

  // ---- reader side (passive broadcast cursor) -----------------------------
  class reader {
  public:
    explicit reader(axe_buffer &b) : buf_(&b) {
      pos_ = buf_->freed_.load(std::memory_order_acquire); // start at oldest
    }

    // ok: out written, cursor advanced. empty: caught up. lapped: freed crossed
    // the cursor during the copy (the item is gone), cursor resyncs to oldest,
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
      const pos_t added = buf_->added_.load(std::memory_order_acquire);
      if (pos_ == added)
        return read_result::empty;

      const size_t idx = buf_->index(pos_);
      atomic_load_bytes(&out, buf_->cells_[idx].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire); // copy before the freed load
      const pos_t freed = buf_->freed_.load(std::memory_order_relaxed);

      if (signed_distance(freed, pos_) < 0) {
        pos_ = freed;
        return read_result::lapped;
      }
      ++pos_;
      return read_result::ok;
    }

    // Headroom, in writes, before the item at the cursor (the next one read()
    // would return) could be overwritten: signed_distance(freed, cursor). It is
    // the item's depth above the reclaim edge: 0 for the oldest live item, up to
    // capacity-1 for the newest, negative once lapped. This is a snapshot that
    // can only shrink as the writer advances; the library holds no clock, so a
    // caller with a bounded read/processing time and a max write rate converts
    // its own time budget into a write-count margin (see DESIGN.md §3). Works
    // for any T (it inspects only the counters, never the cells).
    int64_t remaining_writes() const {
      const pos_t freed = buf_->freed_.load(std::memory_order_acquire);
      return signed_distance(freed, pos_);
    }

    // Guarded read: like read(out), but declines with would_overwrite (leaving
    // the cursor put) when the cursor's item has fewer than min_margin writes of
    // headroom. An already-lapped cursor still reports lapped and resyncs. The
    // seqlock still guarantees the copy is untorn, so min_margin is purely the
    // caller's temporal policy: it never affects correctness for trivial T, only
    // which items are handed back. Trivially-copyable T only.
    read_result read(T &out, uint64_t min_margin) {
      static_assert(std::is_trivially_copyable_v<T>,
                    "reader::read requires trivially-copyable T; use "
                    "unsafe_window otherwise");
      const pos_t added = buf_->added_.load(std::memory_order_acquire);
      if (pos_ == added)
        return read_result::empty;

      const pos_t freed = buf_->freed_.load(std::memory_order_acquire);
      const int64_t margin = signed_distance(freed, pos_);
      if (margin < 0) { // already reclaimed
        pos_ = freed;
        return read_result::lapped;
      }
      if (static_cast<uint64_t>(margin) < min_margin)
        return read_result::would_overwrite; // exists, but too close to the edge

      const size_t idx = buf_->index(pos_);
      atomic_load_bytes(&out, buf_->cells_[idx].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      const pos_t freed2 = buf_->freed_.load(std::memory_order_relaxed);
      if (signed_distance(freed2, pos_) < 0) {
        pos_ = freed2;
        return read_result::lapped;
      }
      ++pos_;
      return read_result::ok;
    }

  private:
    axe_buffer *buf_;
    pos_t pos_ = 0;
  };

  reader new_reader() { return reader(*this); }

  // Consistent oldest-to-newest copy of the live window. Trivially-copyable T
  // only.
  std::vector<T> snapshot() const {
    static_assert(std::is_trivially_copyable_v<T>,
                  "snapshot requires trivially-copyable T; use unsafe_window otherwise");
    for (;;) {
      const pos_t f1 = freed_.load(std::memory_order_acquire);    // read freed first
      const pos_t added = added_.load(std::memory_order_acquire); // ⇒ added >= f1
      const range live{f1, added};
      const uint64_t n = live.size();
      std::vector<T> out(static_cast<size_t>(n));
      for (uint64_t k = 0; k < n; ++k)
        atomic_load_bytes(&out[static_cast<size_t>(k)], cells_[index(live.at(k))].mem,
                          sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire); // copy before the freed recheck
      // freed unchanged ⇒ no copied cell was freed/overwritten during the copy.
      if (freed_.load(std::memory_order_relaxed) == f1)
        return out;
    }
  }

  // Up to two contiguous reference-segments over the live region, plus the freed
  // counter it was observed under. UNSAFE for non-trivial T: references stay
  // valid only while the writer cannot lap the reader (quiescent writer, or
  // sized so the window can't advance by `capacity` during use). Call
  // still_valid(word) AFTER reading to detect (not prevent) a lap and discard
  // the result; reading a cell mid-reconstruction is UB.
  struct unsafe_view {
    mdata_view<T, 2> segs;
    uint64_t word;
  };

  unsafe_view unsafe_window() {
    const pos_t f = freed_.load(std::memory_order_acquire);
    const pos_t added = added_.load(std::memory_order_acquire);
    const range live{f, added};
    unsafe_view uv;
    uv.word = f; // lap sentinel: a lap can only advance freed
    // naked_block<T> is just aligned T-storage, so cells address like a plain
    // T[].
    T *const base = reinterpret_cast<T *>(cells_.get());
    const size_t a = index(live.begin());
    const size_t b = index(live.end());
    if (live.empty()) {
    } else if (a < b) {
      uv.segs[0] = {base + a, base + b};
    } else {
      uv.segs[0] = {base + a, base + cap_};
      uv.segs[1] = {base + 0, base + b};
    }
    return uv;
  }

  bool still_valid(uint64_t observed) const {
    return freed_.load(std::memory_order_acquire) == observed;
  }

private:
  // Per-byte relaxed atomics make concurrent reader/writer cell access race-free;
  // the counters' acquire/release plus the reader's acquire fence give ordering.
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
        // Fresh slot: nothing to destroy, so a throw is already safe: neither
        // counter has advanced and no existing object was touched.
        T &ref = cells_[idx].emplace(std::forward<Args>(args)...);
        ++constructed_count_; // grows over [0, capacity) in warmup, then pinned
        ++staged_;
        return ref;
      }
      // Reuse overwrites in place, so the old object must be destroyed before
      // the new one occupies the same address. Build the replacement in a
      // temporary FIRST: a throwing element constructor then leaves the existing
      // object untouched (strong guarantee, no half-overwritten hole for
      // teardown or a later reuse to trip over). Only once the temporary exists
      // do we destroy and move it in; move_if_noexcept keeps that final step
      // non-throwing whenever T has a noexcept move or a copy, i.e. for
      // effectively every broadcastable type.
      T tmp(std::forward<Args>(args)...); // throw => cell unchanged, not counted
      cells_[idx].destroy();
      T &ref = cells_[idx].emplace(std::move_if_noexcept(tmp));
      ++staged_;
      return ref;
    }
  }

  const size_t cap_;                          // fixed capacity, set at construction
  std::unique_ptr<naked_block<T>[]> cells_;   // one heap allocation, never resized
  std::atomic<pos_t> added_;                  // committed sequence count
  std::atomic<pos_t> freed_;                  // freed sequence count
  std::mutex mutex_;                          // serialises writers
  size_t constructed_count_ = 0;              // # constructed cells = prefix [0, count); teardown only
  size_t staged_ = 0;                         // successful constructs in the current lock; mutex-protected
};

} // namespace axe
