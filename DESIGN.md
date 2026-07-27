# axe_buffer — concurrency & type-safety model

Status: design note (no implementation implied). Captures the reader-safety
model, the two tiers of type support, the temporal-safety tools, and the exact
conditions under which each guarantee holds or breaks.

---

## 1. The core constraint

The defining requirement is **passive readers**: a reader never writes to any
shared state (its cursor is thread-local). This is stronger than lock-free — the
writer cannot even *observe* that a reader exists.

Two consequences follow directly:

- The writer can never know when a slot is unreferenced, so reader safety must be
  **detection, not prevention**: a reader reads optimistically and copes with the
  writer having mutated the slot underneath it.
- For detection to be sound, the speculative read must be **safe even when it
  observes a fully inconsistent, half-written state** — because the reader only
  discovers the tear *after* the read has happened.

Everything below is a consequence of these two facts.

---

## 2. Two mechanisms for reader safety

There are exactly two ways to make a passive-reader read safe. They are not
competitors; one is the safe default, the other extends coverage.

### 2a. Detection — the `freed_` seqlock

Copy the cell, then check `freed_` to learn whether the writer reclaimed the slot
during the copy. If it did, discard the result (`lapped`) and resync.

- **Requires** that a torn read be *harmless to have performed* — i.e. copying an
  arbitrary byte pattern is well-defined and touches no invariants. That is the
  definition of **trivially copyable**.
- **Failure is recoverable**: a tear becomes `lapped`, never corruption.

### 2b. Temporal — a proven grace period

Instead of detecting reuse, *prove it cannot happen during the read*. If you have
a hard upper bound on the writer's rate and a hard upper bound on the reader's
read duration, you can guarantee the slot the reader is touching will not be
reused before the reader is done.

This is grace-period / epoch reclamation with the epoch derived from wall-clock
and a rate bound instead of from readers checking in. It works for **any type**,
because the reader never touches a slot the writer might concurrently mutate.

- **Requires** the timing bounds to actually hold (see §5).
- **Failure is NOT recoverable for non-trivial types**: a missed bound means the
  copy-ctor already ran over a half-destroyed object — silent UB, nothing left to
  detect.

---

## 3. The margin predicate

Temporal safety is **per-value**, not a global buffer property. A common mistake
is "600 values at 1/s ⇒ 599 s of safety" — that is true only for the *newest*
value. The oldest value in a full ring is reused by the *very next* write.

**The library measures margin in *writes*, never in time.** It holds no clock and
observes no rate (see Decisions). The core primitive is:

```
remaining_writes(p) = signed_distance(freed_, p)   // depth of p above the reclaim edge, in writes
```

- Newest value: margin ≈ `S − 1` writes.
- Oldest value: margin ≈ `0`.

Time and rate live entirely on the **caller** side. A caller who knows its own
read-time bound `T_read`, a hard `max_write_rate`, and a safety factor `k ≥ 1`
converts once, in its own units, to a required write-count margin:

```
min_margin = ceil( k · T_read · max_write_rate )     // computed by the caller
read is safe  ⇔  remaining_writes(p) ≥ min_margin
```

The library only ever compares `remaining_writes(p)` against a caller-supplied
`min_margin`. It never learns what a second is.

**Design pattern that falls out of this:** oversize the ring and consume only the
fresh tail (last `n ≪ S` items). Every read then has margin `≥ S − n` writes,
turning "keep the buffer big" into a provable bound rather than a rule of thumb.

`signed_distance(freed_, p)` is already computed by the modular-arithmetic layer,
so the query is nearly free to expose.

---

## 4. Type-support tiers

Two tiers, not three. The temporal tools are **always compiled in** (zero cost
when unused) — there is no separate "fast" tier, because instrumentation that
costs nothing when idle needs no gate.

### Tier A — trivially copyable (default, safe)

- `static_assert(is_trivially_copyable_v<T>)` on the safe read paths (already the
  case for `read()` / `snapshot()`).
- Correctness rests on the seqlock; timing assumptions are **not** required.
- The temporal tools (§3) are available purely as an *optimization*: use the
  margin to drive down the lap rate and choose which values to read. A wrong
  timing estimate here costs a `lapped`, never correctness.
- **No proof obligation on the user.**

### Tier B — non-trivial T (explicit opt-in, temporal)

- Loud, explicitly-named opt-in. The user supplies `max_write_rate` and their
  read-time bound; safety is *their* proof, enforced by §3.
- A missed bound is **UB, not `lapped`**. This must be stated at the call site,
  not buried in docs.
- Keep a post-copy `freed_` recheck as a **debug-build poison detector**: it
  cannot prevent a crash if the copy-ctor itself faults, but for non-faulting
  torn reads it flags corruption in tests — exactly where a too-tight margin
  should be discovered.
- Prefer **copy-out over in-place references** for non-trivial reads: copying
  bounds `T_read` to the copy-ctor duration, whereas holding `unsafe_window`
  references extends the window you must prove closed to the whole processing
  time.

---

## 5. Where the guarantee holds or breaks

| | Tier A (trivial + seqlock) | Tier B (non-trivial + temporal) |
|---|---|---|
| Timing bound holds | correct | correct |
| Timing bound violated | torn value → **detected**, recovered as `lapped` | **silent UB** (copy-ctor ran over a mutating object) |

The timing bound is only as good as the environment:

- **Hard-boundable** (isolated cores, `SCHED_FIFO`, no paging/swap, busy-poll,
  RTOS): `T_read` has a real upper bound → Tier B is genuinely safe with a sane
  margin. "A 599 s stall means the system is already dead" is a valid argument
  *here*.
- **Statistically-boundable only** (general preemptive scheduling, paging, VMs,
  live migration): no hard upper bound on `T_read` exists. Tier B is then a
  *statistical* guarantee — acceptable for Tier A (failure = `lapped`), a footgun
  for Tier B (failure = UB).

The library cannot detect which world it is in, so it must make the user declare
it (choosing Tier B *is* that declaration).

---

## 6. What actually makes a torn read dangerous

(The recurring "isn't it only pointers?" question.)

Pointer dereference is the **dominant and most lethal** risk, but the precise
property is not "does the type contain a pointer." Two clarifications:

1. **The risk is in *consuming* a torn value, not in *storing/copying* it.** A
   trivially-copyable struct that contains a raw pointer is safe to seqlock-copy —
   the copy is a `memcpy` that never dereferences it. `std::string` is unsafe not
   because it holds a pointer but because its *copy constructor* dereferences one
   during the copy.

2. **In Tier A the type's contents are irrelevant**, because the seqlock validates
   *before* the reader consumes the copy. A torn copy — pointer, bad index,
   anything — is discarded as `lapped` before any code touches it. The pointer
   question only matters when consuming **un-validated** data: a non-trivial
   copy-ctor (Tier B), in-place reference reads (`unsafe_window`), or a
   deliberately-unchecked fast read.

For those un-validated cases, "no pointers ⇒ low risk" is *mostly* right but
incomplete. A pointer-free torn value still causes UB when used to:

- index / size / offset a memory access (`data[torn_n]`),
- select a union member or a `switch` assumed exhaustive,
- form an invalid `bool` / `enum` / reference,
- divide (torn zero → SIGFPE).

**Tear-safety property.** A value is safe to consume torn iff no bit pattern of it
can be used to *address memory or pick a branch*. "Flat struct of scalars consumed
as opaque values" satisfies this; pointer deref is simply its most common
violation.

---

## 7. API surface (delivered)

- **Kept** the trivially-copyable `static_assert` on `read()` / `snapshot()`.
- **No rate, no clock.** The library takes no `max_write_rate` and holds no clock.
  All time/rate reasoning is the caller's; the library speaks only in writes.
- **`reader::remaining_writes()`** → `signed_distance(freed_, cursor)`: the
  headroom in writes before the cursor's item is reclaimed (0 = oldest,
  up to S-1 = newest, negative = lapped). Reads only the counters, so it works
  for any `T` (usable alongside `unsafe_window` for the Tier-B reference case).
- **`reader::read(out, min_margin)`** — guarded read: returns `ok` only when
  `remaining_writes() ≥ min_margin`, `would_overwrite` when the item exists but
  is too close to the edge (cursor left in place), and still `lapped`/`empty`
  otherwise. The caller derives `min_margin` from its own `T_read`, rate, and
  safety factor (§3); the library never sees those. For trivially-copyable `T`
  the seqlock still guarantees an untorn copy, so `min_margin` is pure policy —
  it changes *which* items are handed back, never correctness.
- **`read_result::would_overwrite`** — fourth outcome, distinct from `lapped`
  (already gone). The caller decides: skip, widen the ring, or accept via the
  unguarded `read(out)`.

Not yet built (follow-ons, see §8):
- A non-trivial *copy-out* guarded read (copy-construct under the margin gate +
  debug poison detector). Today Tier-B temporal reads use `unsafe_window` +
  `remaining_writes()`; a cursor-based copy-out reader would be more ergonomic.
- A seek-to-recent cursor for the "oversize the ring, consume the fresh tail"
  pattern — currently a reader starts at the oldest and a high `min_margin` on an
  old cursor simply parks it on `would_overwrite`.

---

## 8. Decisions

Resolved:

- **Clock-free.** The core holds no clock and does no time conversion. Margin is
  counted in writes; the caller applies its own rate at the edge. Keeps the header
  dependency-free and deterministically testable.
- **No rate measurement.** The library never measures or estimates the write rate
  (no EWMA). A hard rate bound, where one exists, is the caller's to assert and
  apply. The library only compares write-count margins.
- **`would_overwrite` result.** The guarded read declines with a distinct
  `would_overwrite` result rather than blocking, spinning, or masquerading as
  `empty`. The caller owns the policy.

- **Commit to Tier B.** Non-trivial support stays and is made honest, rather than
  being deleted. That means the write-side lifetime defects (partial fill,
  throwing ctor) are fixed, not documented-around.

  *Delivered:* commit now publishes only the contiguous prefix of slots actually
  constructed (`staged_`), and teardown destroys the explicit constructed prefix
  (`constructed_count_`) rather than inferring it from the live-window size. This
  closes partial fill (any tier) and a throw on a *fresh* slot.

  *Reuse-throw edge — resolved (build-then-move).* Overwriting an existing
  (non-trivial) slot must destroy the old object before the new one occupies the
  same address. The reuse path now builds the replacement in a **temporary
  first**; a throwing element constructor therefore throws before anything is
  destroyed, leaving the existing object untouched (strong guarantee, no hole).
  Only once the temporary exists is the old object destroyed and the temporary
  moved in via `std::move_if_noexcept` — the same idiom `std::vector` uses on
  reallocation: a noexcept move when available, else a copy. This keeps the final
  placement non-throwing for effectively every broadcastable type and needs no
  per-cell construction tracking.

  *Type requirement (Tier-B).* A non-trivial element must be move- or
  copy-constructible (`static_assert`-enforced). This is not a real restriction:
  a ring that overwrites its cells cannot hold an un-copyable, un-movable
  payload.

  *Residual pathological case.* The only remaining hole is a type that has **no
  non-throwing way to be relocated** — i.e. `move_if_noexcept` is forced to
  select a throwing operation (its move is not `noexcept` *and* its copy, if any,
  throws). Then the final in-place step can throw after the old object is already
  destroyed. Such a type as an overwriting broadcast payload is vanishingly rare;
  covering it too would require per-cell construction tracking (option C — a
  one-bit-per-cell constructed flag), which is deliberately not paid for here.
  Documented, not silently unsafe.

- **Temporal read tools — delivered.** `remaining_writes()`, the guarded
  `read(out, min_margin)`, and `read_result::would_overwrite` are implemented on
  the reader (§7). Clock-free, write-count margins, `would_overwrite` distinct
  from `lapped`.

- **Runtime capacity — delivered.** Capacity moved from a template parameter to a
  constructor argument (`axe_buffer<T> buf(n)`), so it can be config-driven.
  Storage is one heap allocation (`unique_ptr<naked_block<T>[]>`), in-process
  only (not shared memory). The concurrency core (fences, seqlock,
  free-before-write) was unchanged; only the arithmetic representation and
  storage moved. `mod_int`/`mod_index`/`mod_range` were collapsed into a few
  private helpers (`index`, `add_mod`, `distance`, `signed_dist`) on the class.

- **Runtime modulus, `word_t` restored.** `word_t` is again a template parameter
  (default `uint64_t`). Counters run modulo `M` = the largest multiple of
  `capacity` that fits `word_t` (0 = natural `2^N` wrap for power-of-two
  capacity), computed at construction. Because `M` is a multiple of `capacity`,
  `pos % capacity` is exact across the wrap for *any* capacity, so the ring is
  correct for unbounded additions — the `2^64` (or `2^N`) caveat from the
  natural-wrap interim is gone. Smaller `word_t` is fully supported and unit-
  tested across wraps, subject to the `capacity <= max(word_t)/2` bound and the
  reader not lagging more than `M/2` between reads (documented; not a concern for
  `uint64_t`).

- **`constructed_count_` and `staged_` removed as members.** The free is now
  per-cell inside `construct_at` (build the replacement, publish `freed` +
  fence, then overwrite), instead of eagerly in `writer_lock`. That keeps `freed`
  from ever running ahead of what was actually written, so there are no orphan
  cells: the constructed set is always the prefix `[0, added - freed)`, and the
  destructor derives its count from that lag directly. `staged_` (the committed
  prefix length) now lives in `writer_range`. Side benefit: a throwing multi-slot
  lock now evicts only what it actually replaced, losing less data on partial
  failure. `was_full` was also dropped — reuse is `distance(freed, pos) >=
  capacity`.

Still open (follow-ons):

- **Non-trivial copy-out guarded read** with the debug poison detector (§7).
- **Seek-to-recent cursor** for the oversize-and-tail pattern (§7, §3).
