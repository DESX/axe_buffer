#pragma once
// Single-header SPMC (one writer / many readers) FIFO ring, runtime-sized.
// Design notes: https://github.com/DESX/axe_buffer

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace axe {

// Aligned uninitialised storage for one T, constructed/destroyed on demand.
template <typename T> struct naked_block {
  template <typename... Args> T &emplace(Args &&...args) {
    return *(::new (static_cast<void *>(mem)) T(std::forward<Args>(args)...));
  }
  T &ref() { return *std::launder(reinterpret_cast<T *>(mem)); }
  void destroy() { ref().~T(); }
  alignas(T) unsigned char mem[sizeof(T)];
};

// ok/empty/lapped are ordinary read outcomes. would_overwrite comes only from
// the guarded read: the item is live but has too little headroom, so it was
// declined and the cursor left in place (distinct from lapped: already gone).
enum class read_result { ok, empty, lapped, would_overwrite };

template <typename T> class axe_buffer {
  // Monotonic sequence counter; wraps at 2^64 (centuries away at any real rate,
  // so pos % capacity is exact in practice for every capacity).
  using pos_t = uint64_t;

  static size_t validate_capacity(size_t c) {
    if (c == 0)
      throw std::invalid_argument("axe_buffer capacity must be > 0");
    return c;
  }

  size_t index(pos_t p) const { return static_cast<size_t>(p % cap_); }

  // Signed cyclic distance a->b; correct while |b - a| < 2^63, always true here
  // (only positions within the live window, at most `capacity` apart, compare).
  static int64_t signed_distance(pos_t a, pos_t b) {
    return static_cast<int64_t>(b - a);
  }

  // Half-open [begin, end) span of sequence positions (the live region, and each
  // writer's staged span). Cell indexing happens via index() at access time.
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

  // Consistent because the writer holds the mutex and is the only mutator.
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
    // Constructed cells are the prefix [0, constructed_count_): warmup fills
    // 0,1,2,… and reuse reconstructs in place, so the set never gaps. Using the
    // explicit count (not the live size) stays correct when a partial or aborted
    // lock leaves freed ahead of added.
    if constexpr (!std::is_trivially_copyable_v<T>) {
      for (size_t i = 0; i < constructed_count_; ++i)
        cells_[i].destroy();
    }
  }

  axe_buffer(const axe_buffer &) = delete;
  axe_buffer &operator=(const axe_buffer &) = delete;

  // ---- writer side --------------------------------------------------------
  class writer_range;

  // Reserve n tail slots (freeing front space first when full). Returns a
  // move-only RAII handle holding the writer mutex; slots become visible on
  // commit() or when the handle leaves scope.
  writer_range writer_lock(size_t n) {
    mutex_.lock();
    staged_ = 0;
    range live = live_relaxed();
    if (n > cap_)
      n = cap_; // clamp; n == 0 is a valid no-op

    const size_t sz = static_cast<size_t>(live.size());
    const bool was_full = (sz == cap_);
    const size_t need_free = (sz + n > cap_) ? (sz + n - cap_) : 0;
    if (need_free) {
      // Free-before-write: publish the freed advance (release) and fence BEFORE
      // any freed cell is overwritten, so a reader's post-copy freed load sees
      // the advance and reports the tear.
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

  // reuse ⇔ the target cell already holds a constructed object (destroyed first
  // for non-trivial T): the ring was full at lock time, or this is a wrapped
  // position. was_full pins reuse once the ring fills.
  slot_proxy stage_slot(pos_t pos, bool was_full) {
    return slot_proxy(*this, index(pos), was_full || pos >= cap_);
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
    slot_proxy operator[](size_t i) const { return buf_->stage_slot(span_.at(i), was_full_); }

    // Idempotent; runs on scope exit. Publishes only the contiguous prefix
    // actually constructed (staged_, capped at the span), so a partial fill or a
    // mid-lock exception never exposes an unwritten cell. One release store
    // publishes every staged cell write that precedes it.
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
    // the cursor during the copy, cursor resyncs, out untouched. Trivial T only.
    //
    // No retry: freed is monotonic, so one post-copy check that freed <= pos
    // proves it held for the whole copy ⇒ the bytes were not overwritten. The
    // acquire fence orders the copy before the freed load.
    read_result read(T &out) {
      static_assert(std::is_trivially_copyable_v<T>,
                    "reader::read requires trivially-copyable T; use unsafe_window otherwise");
      const pos_t added = buf_->added_.load(std::memory_order_acquire);
      if (pos_ == added)
        return read_result::empty;
      atomic_load_bytes(&out, buf_->cells_[buf_->index(pos_)].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      const pos_t freed = buf_->freed_.load(std::memory_order_relaxed);
      if (signed_distance(freed, pos_) < 0) {
        pos_ = freed;
        return read_result::lapped;
      }
      ++pos_;
      return read_result::ok;
    }

    // Writes of headroom before the cursor's item is overwritten: 0 for the
    // oldest live item, up to capacity-1 for the newest, negative once lapped.
    // The library holds no clock; the caller turns its own read-time budget and
    // write-rate bound into this write-count margin (DESIGN.md §3). Any T.
    int64_t remaining_writes() const {
      return signed_distance(buf_->freed_.load(std::memory_order_acquire), pos_);
    }

    // Guarded read: declines with would_overwrite (cursor unmoved) when the item
    // has fewer than min_margin writes of headroom; otherwise like read(out).
    // The seqlock still guarantees an untorn copy, so min_margin is pure caller
    // policy, never correctness. Trivial T only.
    read_result read(T &out, uint64_t min_margin) {
      static_assert(std::is_trivially_copyable_v<T>,
                    "reader::read requires trivially-copyable T; use unsafe_window otherwise");
      const pos_t added = buf_->added_.load(std::memory_order_acquire);
      if (pos_ == added)
        return read_result::empty;
      const pos_t freed = buf_->freed_.load(std::memory_order_acquire);
      const int64_t margin = signed_distance(freed, pos_);
      if (margin < 0) {
        pos_ = freed;
        return read_result::lapped;
      }
      if (static_cast<uint64_t>(margin) < min_margin)
        return read_result::would_overwrite;
      atomic_load_bytes(&out, buf_->cells_[buf_->index(pos_)].mem, sizeof(T));
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

  // Consistent oldest-to-newest copy of the live window. Trivial T only.
  std::vector<T> snapshot() const {
    static_assert(std::is_trivially_copyable_v<T>,
                  "snapshot requires trivially-copyable T; use unsafe_window otherwise");
    for (;;) {
      const pos_t f1 = freed_.load(std::memory_order_acquire); // freed first ⇒ added >= f1
      const pos_t added = added_.load(std::memory_order_acquire);
      const range live{f1, added};
      const uint64_t n = live.size();
      std::vector<T> out(static_cast<size_t>(n));
      for (uint64_t k = 0; k < n; ++k)
        atomic_load_bytes(&out[static_cast<size_t>(k)], cells_[index(live.at(k))].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      if (freed_.load(std::memory_order_relaxed) == f1) // unchanged ⇒ nothing was reclaimed mid-copy
        return out;
    }
  }

  // Up to two contiguous spans over the live region, plus the freed value they
  // were read under. UNSAFE for non-trivial T: the spans stay valid only while
  // the writer cannot lap the region (quiescent writer, or a margin per §3).
  // Call still_valid(word) AFTER use to detect (not prevent) a lap.
  struct unsafe_view {
    std::array<std::span<T>, 2> segs;
    uint64_t word;
  };

  unsafe_view unsafe_window() {
    const pos_t f = freed_.load(std::memory_order_acquire);
    const pos_t added = added_.load(std::memory_order_acquire);
    const range live{f, added};
    T *const base = reinterpret_cast<T *>(cells_.get()); // naked_block<T> aliases T storage
    const size_t a = index(live.begin());
    const size_t b = index(live.end());
    unsafe_view uv;
    uv.word = f; // a lap can only advance freed
    if (live.empty()) {
    } else if (a < b) {
      uv.segs[0] = std::span<T>(base + a, base + b);
    } else {
      uv.segs[0] = std::span<T>(base + a, base + cap_);
      uv.segs[1] = std::span<T>(base, base + b);
    }
    return uv;
  }

  bool still_valid(uint64_t observed) const {
    return freed_.load(std::memory_order_acquire) == observed;
  }

private:
  // Per-byte relaxed atomics make concurrent cell access race-free; ordering
  // comes from the counters' acquire/release and the reader's acquire fence.
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

  // Trivial T: race-free byte store. Non-trivial T: placement-new for a fresh
  // slot, or build-a-temporary-then-move-in-place for reuse so a throwing
  // element constructor cannot leave a destroyed hole. staged_/constructed_count_
  // advance only after a successful construction.
  template <typename... Args> T &construct_cell(size_t idx, bool reuse, Args &&...args) {
    if constexpr (std::is_trivially_copyable_v<T>) {
      (void)reuse;
      T tmp(std::forward<Args>(args)...); // may throw before anything is published
      atomic_store_bytes(cells_[idx].mem, &tmp, sizeof(T));
      ++staged_;
      return cells_[idx].ref();
    } else {
      static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                    "non-trivial axe_buffer element must be move- or copy-constructible");
      if (!reuse) {
        T &ref = cells_[idx].emplace(std::forward<Args>(args)...);
        ++constructed_count_;
        ++staged_;
        return ref;
      }
      T tmp(std::forward<Args>(args)...); // build first: a throw leaves the old object intact
      cells_[idx].destroy();
      T &ref = cells_[idx].emplace(std::move_if_noexcept(tmp));
      ++staged_;
      return ref;
    }
  }

  const size_t cap_;                        // fixed capacity, set at construction
  std::unique_ptr<naked_block<T>[]> cells_; // one heap allocation, never resized
  std::atomic<pos_t> added_;                // committed sequence count
  std::atomic<pos_t> freed_;                // freed sequence count
  std::mutex mutex_;                        // serialises writers
  size_t constructed_count_ = 0;            // # constructed cells = prefix [0, count); teardown
  size_t staged_ = 0;                       // successful constructs in the current lock
};

} // namespace axe
