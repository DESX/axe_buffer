#pragma once
// Single-header SPMC (one writer / many readers) FIFO ring, runtime-sized.
// Design notes: https://github.com/DESX/axe_buffer

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace axe {

// Release version, stamped into the distributable header at build time (the
// Makefile injects AXE_BUFFER_VERSION); a dev marker otherwise. Available both
// as the macro and as axe::version.
#ifndef AXE_BUFFER_VERSION
#define AXE_BUFFER_VERSION "0.0.0-dev"
#endif
inline constexpr std::string_view version = AXE_BUFFER_VERSION;

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

// word_t is the unsigned counter/position type. A wider type pushes the wrap
// boundary further out; the default never wraps in practice. Smaller types wrap
// often and are fully supported (and exercised by the unit tests).
template <typename T, typename word_t = uint64_t> class axe_buffer {
  static_assert(std::is_unsigned_v<word_t>, "word_t must be an unsigned integer");

  static size_t validate_capacity(size_t c) {
    if (c == 0)
      throw std::invalid_argument("axe_buffer capacity must be > 0");
    if (c > static_cast<size_t>(std::numeric_limits<word_t>::max() / 2))
      throw std::invalid_argument("axe_buffer capacity too large for word_t");
    return c;
  }

  // Counters run modulo M_: the largest multiple of capacity that fits word_t,
  // or 0 (natural 2^N wrap) when capacity is a power of two. A multiple of
  // capacity keeps pos % capacity exact across the wrap.
  static word_t compute_modulus(size_t cap) {
    const word_t c = static_cast<word_t>(cap);
    if ((c & static_cast<word_t>(c - 1)) == 0)
      return 0; // power of two ⇒ natural word_t wrap already aligns
    const word_t mx = std::numeric_limits<word_t>::max();
    return static_cast<word_t>(mx - mx % c);
  }

  size_t index(word_t p) const { return static_cast<size_t>(p % static_cast<word_t>(cap_)); }

  // a + k (mod M_). On word_t overflow, gap_ re-aligns the wrap from 2^N to M_.
  word_t add_mod(word_t a, word_t k) const {
    if (M_ == 0)
      return static_cast<word_t>(a + k);
    word_t s = static_cast<word_t>(a + k);
    if (s < a)
      s = static_cast<word_t>(s + gap_);
    return static_cast<word_t>(s % M_);
  }

  // (to - from) mod M_, in [0, M_): the forward gap, e.g. the live-window size.
  word_t distance(word_t from, word_t to) const {
    if (M_ == 0)
      return static_cast<word_t>(to - from);
    const word_t f = from % M_, t = to % M_;
    return t >= f ? static_cast<word_t>(t - f) : static_cast<word_t>(M_ - (f - t));
  }

  // Signed cyclic distance a->b, in (-M_/2, M_/2] (or the word_t range when M_==0).
  int64_t signed_dist(word_t a, word_t b) const {
    if (M_ == 0)
      return static_cast<int64_t>(
          static_cast<std::make_signed_t<word_t>>(static_cast<word_t>(b - a)));
    const word_t fwd = distance(a, b);
    return fwd > M_ / 2 ? static_cast<int64_t>(fwd) - static_cast<int64_t>(M_)
                        : static_cast<int64_t>(fwd);
  }

public:
  using value_type = T;
  size_t capacity() const { return cap_; }

  explicit axe_buffer(size_t capacity)
      : cap_(validate_capacity(capacity)), M_(compute_modulus(cap_)),
        gap_(M_ ? static_cast<word_t>(std::numeric_limits<word_t>::max() - M_ + 1) : 0),
        cells_(std::make_unique<naked_block<T>[]>(cap_)), added_(0), freed_(0) {}

  ~axe_buffer() {
    // Constructed cells are the prefix [0, live size): the free is per-cell (see
    // construct_at), so freed never runs ahead of what was written and the
    // constructed set never gaps. Live size = distance(freed, added).
    if constexpr (!std::is_trivially_copyable_v<T>) {
      const size_t n = static_cast<size_t>(
          distance(freed_.load(std::memory_order_relaxed), added_.load(std::memory_order_relaxed)));
      for (size_t i = 0; i < n; ++i)
        cells_[i].destroy();
    }
  }

  axe_buffer(const axe_buffer &) = delete;
  axe_buffer &operator=(const axe_buffer &) = delete;

  // ---- writer side --------------------------------------------------------
  class writer_range;

  // Reserve n tail slots. Returns a move-only RAII handle holding the writer
  // mutex; slots become visible on commit() or when the handle leaves scope.
  writer_range writer_lock(size_t n) {
    mutex_.lock();
    if (n > cap_)
      n = cap_; // clamp; n == 0 is a valid no-op
    return writer_range(*this, added_.load(std::memory_order_relaxed), n);
  }

  template <typename... Args> void push_back(Args &&...args) {
    auto r = writer_lock(1);
    r[0].emplace(std::forward<Args>(args)...);
  }

  class slot_proxy {
  public:
    slot_proxy(axe_buffer &b, word_t pos, size_t *staged) : buf_(b), pos_(pos), staged_(staged) {}
    template <typename... Args> T &emplace(Args &&...args) {
      T &r = buf_.construct_at(pos_, std::forward<Args>(args)...);
      ++*staged_; // only on success; construct_at propagates a throw untouched
      return r;
    }
    template <typename U> T &operator=(U &&v) { return emplace(std::forward<U>(v)); }

  private:
    axe_buffer &buf_;
    word_t pos_;
    size_t *staged_;
  };

  class writer_range {
  public:
    class iterator {
    public:
      iterator(axe_buffer &b, word_t pos, size_t *staged) : buf_(&b), pos_(pos), staged_(staged) {}
      bool operator!=(const iterator &o) const { return pos_ != o.pos_; }
      iterator &operator++() {
        pos_ = buf_->add_mod(pos_, 1);
        return *this;
      }
      slot_proxy operator*() { return slot_proxy(*buf_, pos_, staged_); }

    private:
      axe_buffer *buf_;
      word_t pos_;
      size_t *staged_;
    };

    writer_range(axe_buffer &b, word_t start, size_t n) : buf_(&b), start_(start), count_(n) {}

    // Move-only: owns the writer mutex and the commit obligation.
    writer_range(writer_range &&o) noexcept
        : buf_(o.buf_), start_(o.start_), count_(o.count_), staged_(o.staged_),
          committed_(o.committed_) {
      o.buf_ = nullptr;
    }
    writer_range(const writer_range &) = delete;
    writer_range &operator=(const writer_range &) = delete;
    writer_range &operator=(writer_range &&) = delete;
    ~writer_range() { commit(); }

    size_t size() const { return count_; }
    iterator begin() { return iterator(*buf_, start_, &staged_); }
    iterator end() {
      return iterator(*buf_, buf_->add_mod(start_, static_cast<word_t>(count_)), &staged_);
    }
    slot_proxy operator[](size_t i) {
      return slot_proxy(*buf_, buf_->add_mod(start_, static_cast<word_t>(i)), &staged_);
    }

    // Idempotent; runs on scope exit. Publishes only the contiguous prefix
    // actually constructed (staged_), so a partial fill or a mid-lock exception
    // never exposes an unwritten cell. One release store publishes every staged
    // cell write that precedes it.
    void commit() {
      if (!buf_ || committed_)
        return;
      buf_->added_.store(buf_->add_mod(start_, static_cast<word_t>(staged_)),
                         std::memory_order_release);
      committed_ = true;
      buf_->mutex_.unlock();
    }

  private:
    axe_buffer *buf_;
    word_t start_;
    size_t count_;
    size_t staged_ = 0;
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
      const word_t added = buf_->added_.load(std::memory_order_acquire);
      if (pos_ == added)
        return read_result::empty;
      atomic_load_bytes(&out, buf_->cells_[buf_->index(pos_)].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      const word_t freed = buf_->freed_.load(std::memory_order_relaxed);
      if (buf_->signed_dist(freed, pos_) < 0) {
        pos_ = freed;
        return read_result::lapped;
      }
      pos_ = buf_->add_mod(pos_, 1);
      return read_result::ok;
    }

    // Writes of headroom before the cursor's item is overwritten: 0 for the
    // oldest live item, up to capacity-1 for the newest, negative once lapped.
    // The library holds no clock; the caller turns its own read-time budget and
    // write-rate bound into this write-count margin (DESIGN.md §3). Any T.
    int64_t remaining_writes() const {
      return buf_->signed_dist(buf_->freed_.load(std::memory_order_acquire), pos_);
    }

    // Guarded read: declines with would_overwrite (cursor unmoved) when the item
    // has fewer than min_margin writes of headroom; otherwise like read(out).
    // The seqlock still guarantees an untorn copy, so min_margin is pure caller
    // policy, never correctness. Trivial T only.
    read_result read(T &out, uint64_t min_margin) {
      static_assert(std::is_trivially_copyable_v<T>,
                    "reader::read requires trivially-copyable T; use unsafe_window otherwise");
      const word_t added = buf_->added_.load(std::memory_order_acquire);
      if (pos_ == added)
        return read_result::empty;
      const word_t freed = buf_->freed_.load(std::memory_order_acquire);
      const int64_t margin = buf_->signed_dist(freed, pos_);
      if (margin < 0) {
        pos_ = freed;
        return read_result::lapped;
      }
      if (static_cast<uint64_t>(margin) < min_margin)
        return read_result::would_overwrite;
      atomic_load_bytes(&out, buf_->cells_[buf_->index(pos_)].mem, sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      const word_t freed2 = buf_->freed_.load(std::memory_order_relaxed);
      if (buf_->signed_dist(freed2, pos_) < 0) {
        pos_ = freed2;
        return read_result::lapped;
      }
      pos_ = buf_->add_mod(pos_, 1);
      return read_result::ok;
    }

  private:
    axe_buffer *buf_;
    word_t pos_ = 0;
  };

  reader new_reader() { return reader(*this); }

  // Consistent oldest-to-newest copy of the live window. Trivial T only.
  std::vector<T> snapshot() const {
    static_assert(std::is_trivially_copyable_v<T>,
                  "snapshot requires trivially-copyable T; use unsafe_window otherwise");
    for (;;) {
      const word_t f1 = freed_.load(std::memory_order_acquire); // freed first ⇒ added >= f1
      const word_t added = added_.load(std::memory_order_acquire);
      const word_t n = distance(f1, added);
      std::vector<T> out(static_cast<size_t>(n));
      for (word_t k = 0; k < n; ++k)
        atomic_load_bytes(&out[static_cast<size_t>(k)], cells_[index(add_mod(f1, k))].mem,
                          sizeof(T));
      std::atomic_thread_fence(std::memory_order_acquire);
      if (freed_.load(std::memory_order_relaxed) ==
          f1) // unchanged ⇒ nothing was reclaimed mid-copy
        return out;
    }
  }

  // Up to two contiguous spans over the live region, plus the freed value they
  // were read under. UNSAFE for non-trivial T: the spans stay valid only while
  // the writer cannot lap the region (quiescent writer, or a margin per §3).
  // Call still_valid(word) AFTER use to detect (not prevent) a lap.
  struct unsafe_view {
    std::array<std::span<T>, 2> segs;
    word_t word;
  };

  unsafe_view unsafe_window() {
    const word_t f = freed_.load(std::memory_order_acquire);
    const word_t added = added_.load(std::memory_order_acquire);
    T *const base = reinterpret_cast<T *>(cells_.get()); // naked_block<T> aliases T storage
    const size_t a = index(f);
    const size_t b = index(added);
    unsafe_view uv;
    uv.word = f; // a lap can only advance freed
    if (f == added) {
    } else if (a < b) {
      uv.segs[0] = std::span<T>(base + a, base + b);
    } else {
      uv.segs[0] = std::span<T>(base + a, base + cap_);
      uv.segs[1] = std::span<T>(base, base + b);
    }
    return uv;
  }

  bool still_valid(word_t observed) const {
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

  // Write one cell. reuse (the ring is full at this position) evicts the oldest
  // first: free-before-write publishes freed (release) and fences BEFORE the
  // overwrite, so a reader's post-copy freed load sees the advance and reports
  // the tear. For non-trivial T the replacement is built first, so a throwing
  // element constructor leaves the existing object and freed untouched.
  template <typename... Args> T &construct_at(word_t pos, Args &&...args) {
    const word_t f = freed_.load(std::memory_order_relaxed);
    const bool reuse = distance(f, pos) >= static_cast<word_t>(cap_);
    const size_t idx = index(pos);
    if constexpr (std::is_trivially_copyable_v<T>) {
      T tmp(std::forward<Args>(args)...); // may throw before anything changes
      if (reuse) {
        freed_.store(add_mod(f, 1), std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
      }
      atomic_store_bytes(cells_[idx].mem, &tmp, sizeof(T));
      return cells_[idx].ref();
    } else {
      static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                    "non-trivial axe_buffer element must be move- or copy-constructible");
      if (!reuse)
        return cells_[idx].emplace(std::forward<Args>(args)...);
      T tmp(std::forward<Args>(args)...); // build first: a throw leaves the old object intact
      freed_.store(add_mod(f, 1), std::memory_order_release);
      std::atomic_thread_fence(std::memory_order_release);
      cells_[idx].destroy();
      return cells_[idx].emplace(std::move_if_noexcept(tmp));
    }
  }

  const size_t cap_;                        // fixed capacity, set at construction
  const word_t M_;                          // counter modulus (0 = natural wrap)
  const word_t gap_;                        // word_t-overflow correction for add_mod
  std::unique_ptr<naked_block<T>[]> cells_; // one heap allocation, never resized
  std::atomic<word_t> added_;               // committed sequence count
  std::atomic<word_t> freed_;               // freed sequence count
  std::mutex mutex_;                        // serialises writers
};

} // namespace axe
