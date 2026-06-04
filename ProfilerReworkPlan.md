# LPC Sampling Profiler — Rework Plan

## Background

The current profiler (`src/lpc_profiler.{c,h}`) samples the LPC call stack
from a `SIGVTALRM` handler at a configurable rate and writes a
flamegraph-compatible "collapsed stacks" file when sampling stops.

The HEAD commit (`wip: copy strings, no dangle ptr`) fixes a real
use-after-free bug: previously the handler stored raw `const char *`
pointers into `string_t` data owned by programs/objects, and those strings
could be freed (via destruct queue processing, GC, `replace_program`,
object rename, or any refcount drop reaching zero) before the writer ran
at `lpc_profile_stop()` time. The fix copies frame/object/program names
into per-sample inline buffers (`LPC_PROFILE_PROG_LEN`,
`LPC_PROFILE_FUNC_LEN`, etc.), but every slot in the
`LPC_PROFILE_MAX_SAMPLES`-sized ring now pays the worst-case copy cost
whether or not the slot is ever used, balloons memory, and limits how
long a profiling session can run in practice.

This document plans a two-phase rework. The goal is bounded memory usage
that scales with **unique stacks**, not **total samples**, while keeping
the dangling-pointer fix correct.

## Out of scope

- A producer-side packed arena to shrink the per-slot copy footprint.
  Considered and deferred — Phase B is expected to make the producer
  ring small enough that the per-slot waste is acceptable. Revisit only
  if Phase B leaves a measurable problem.
- A threaded consumer / lock-free atomics. The driver is single-threaded
  and will remain so; the SPSC ring lives entirely on one thread with
  the signal handler as producer and the backend loop as consumer.
- Streaming output to disk during sampling. Aggregate in memory, write
  at the end.

## Architecture, end state

```
   [interpreter thread]
   ┌──────────────────────────────────────────────────────────────┐
   │                                                              │
   │  SIGVTALRM handler                                           │
   │  ─ walk csp / current_prog / current_interactive             │
   │  ─ copy frame/program/object name bytes into the slot        │
   │  ─ publish slot via SPSC ring tail bump                      │
   │           │                                                  │
   │           ▼                                                  │
   │  ┌─────────────────────┐                                     │
   │  │ producer ring       │   small, fixed (≈256 entries)       │
   │  │ inline-buffer slots │   sized for one tick of samples     │
   │  └─────────────────────┘                                     │
   │           │ drained by backend loop, top of every iteration  │
   │           ▼                                                  │
   │  ┌─────────────────────┐                                     │
   │  │ aggregator          │   stack-arena + open-addressed      │
   │  │   stack_arena       │   hashmap, key=concatenated frame   │
   │  │   hashmap           │   string, value=sample count.       │
   │  │   dropped counter   │   memory grows with unique stacks   │
   │  └─────────────────────┘                                     │
   │           │ lpc_profile_stop() does a final drain,           │
   │           │ then walks the hashmap                           │
   │           ▼                                                  │
   │       collapsed-stacks output file                           │
   │                                                              │
   └──────────────────────────────────────────────────────────────┘
```

## Phase A — Producer/consumer split

### What changes

Introduce an SPSC ring between the signal handler (producer) and a new
drain entry point (consumer) called from the backend loop. The data
model is unchanged: slots still carry inline-buffer copies of names.
The consumer storage is the existing `samples[]` ring with its
existing wrap-on-overflow behavior; the drain becomes its sole writer.
Phase A is a pure architectural split — memory behavior and output are
identical to HEAD when the producer ring doesn't overflow.

### Concrete pieces

1. **Producer ring** in `src/lpc_profiler.c`:
   - Fixed capacity `LPC_PROFILE_PRODUCER_RING` (start with 256, tune
     once Phase A is running).
   - Single-producer (signal handler), single-consumer (drain function
     on backend thread). Indices are `volatile sig_atomic_t` head/tail;
     producer writes slot bytes then bumps tail, consumer reads head
     then slot bytes — sufficient under single-threaded interpreter +
     signal handler semantics.
   - Slot layout: same `lpc_profile_sample_t` as HEAD (inline buffers).

2. **Consumer storage**: the existing `samples[]` ring, untouched.
   HEAD already treats `samples[]` as a fixed-capacity ring that wraps
   on overflow (oldest sample overwritten), tracked via
   `sample_write_idx` and `sample_count`. Phase A keeps that semantics
   exactly. The drain becomes the only writer; the signal handler no
   longer writes here directly.

3. **Drain function** `lpc_profile_drain(void)`:
   - Reads all available producer slots, copies them into the consumer
     `samples[]` ring at `sample_write_idx`, advancing as today.
   - Cheap, no allocation, runs in normal interpreter context.

4. **Drop counter** `lpc_profile_dropped_samples` — for the producer
   ring only:
   - If the handler finds the producer ring full (consumer drain
     hasn't kept up), increment the counter and return without writing
     a slot.
   - This is a *new* failure mode introduced by the SPSC handoff and
     worth surfacing: a non-zero counter means the configured
     producer-ring size is too small for the current sample rate /
     workload, and the profile has gaps that aren't just the existing
     wrap-on-overflow behavior.
   - Emitted at `lpc_profile_stop()` time as a leading comment in the
     output file (e.g. `# dropped N samples: producer ring full`).
   - The existing consumer-ring wrap is **not** counted here — it is
     unchanged from HEAD and its statistical effect is whatever it is
     today.

5. **Drain placement**: top of `backend_main_loop` in `src/backend.c`,
   before `remove_destructed_objects()`. Rationale: lowest latency
   between handler-write and consumer-read, smallest tolerable producer
   ring, and a predictable "drain pending events first, then run the
   loop" ordering. `lpc_profile_stop()` calls `lpc_profile_drain()` once
   itself before writing, so any in-flight samples at stop time make it
   into the output.

### Acceptance criteria for Phase A

- `lpc_profile_start` / `lpc_profile_stop` produce a flamegraph-collapsed
  output byte-for-byte identical to HEAD for an equivalent workload.
- Dropped-sample counter remains zero under a representative workload
  at default sample rate. If non-zero, tune `LPC_PROFILE_PRODUCER_RING`.
- No new allocations from the signal handler.
- AddressSanitizer build of the driver runs the profiler through a
  short workload with no reports.

## Phase B — Aggregating consumer + test harness

### What changes

Replace the consumer accumulator (the session-long raw-sample array)
with a stack-arena + hashmap that **aggregates at drain time**. Each
drained sample is converted into its collapsed-format stack string,
looked up in a hashmap, and either bumps an existing entry's count or
inserts a new entry. The session no longer accumulates raw samples at
all — only unique stacks and their counts.

This is where the memory benefit lands: storage scales with unique
stacks (typically hundreds in a real workload), not total samples
(thousands per minute).

### Concrete pieces

1. **Extract consumer to its own module** `src/lpc_profile_agg.{c,h}`.
   API:
   ```c
   struct lpc_profile_agg;

   struct lpc_profile_agg *lpc_profile_agg_new(void);
   void lpc_profile_agg_free(struct lpc_profile_agg *);

   /* Insert one sample. Frames are pre-formatted (collapsed-format
    * fragments, no trailing delimiter). On allocation failure, returns
    * false; caller treats as a dropped sample.
    */
   bool lpc_profile_agg_insert(struct lpc_profile_agg *,
                               const char *interactive_basename, /* or NULL */
                               const char *root_object_name,     /* or NULL */
                               const char * const *frames,
                               size_t num_frames);

   /* Write collapsed output. Returns bytes written, -1 on I/O error. */
   ssize_t lpc_profile_agg_write(const struct lpc_profile_agg *, int fd);
   ```

2. **Internals**:
   - **Stack arena**: append-only byte buffer; grow via `xrealloc`
     doubling. Holds the concatenated `prefix;frame1;frame2;...` bytes
     for every unique stack. Records `(offset, length)` per stack.
   - **Aggregator hashmap**: open-addressed, power-of-two sized,
     linear probing, load factor 0.5. Entries are
     `{ hash32_t hash; uint32_t arena_offset; uint32_t length; uint64_t count; }`.
     Hash with `hashmem32` over the arena bytes. Key compare with
     `memcmp`. ID-0 sentinel is unused here — empty slots are marked by
     `length == 0` or a separate occupancy bit; final choice when
     writing.
   - **No frame interning.** Decision recorded earlier: the stack arena
     + aggregator combination delivers the bounded-memory property; a
     separate frame interner is additional code without proportionate
     gain at this scale. Reconsider if profiling shows the stack arena
     itself is the memory bottleneck.

3. **Drain logic updated**: `lpc_profile_drain()` now formats each
   producer slot into stack-arena bytes on the fly and calls
   `lpc_profile_agg_insert()`. The session-long raw-sample array from
   Phase A is deleted.

4. **`lpc_profile_stop()`** drains, then calls
   `lpc_profile_agg_write()` to the output file, then frees the
   aggregator.

### Test harness

The aggregator module is the first piece of the profiler that can be
unit-tested in isolation — it takes plain C strings in and produces
collapsed-format bytes out, with no dependency on signals, the
interpreter, or LDMud globals.

**Build setup**:

- New file `test/test_lpc_profile_agg.c`. Compiles standalone against
  `src/lpc_profile_agg.{c,h}` plus a tiny stub for `xalloc` / `xrealloc`
  / `xfree` that maps directly to libc `malloc`/`realloc`/`free`. No
  dependency on the driver build.
- New `test/Makefile` target `test-agg`:
  ```
  test-agg: test_lpc_profile_agg.c ../src/lpc_profile_agg.c
      $(CC) -std=c11 -Wall -Wextra -Werror \
            -fsanitize=address,undefined -g -O1 \
            -I../src $^ -o test_lpc_profile_agg
      ./test_lpc_profile_agg
  ```
- Invocation: `make -C test test-agg`. Runs under ASan+UBSan by default
  so probe wraparound, off-by-one resize, and uninitialised-memory bugs
  surface immediately.

**Coverage**:

The bug classes that matter for this module are: hashmap probe
correctness, resize correctness, arena offset stability, output format
fidelity, and edge cases around empty/truncated samples. Cases:

1. **Single sample, single frame.** Insert one stack, write, verify
   output is exactly `frame 1\n`.
2. **Repeat insertion increments count.** Same stack inserted N times
   → exactly one output line with count N.
3. **Distinct stacks stay distinct.** Two different stacks → two
   output lines, each count 1.
4. **Prefix variation creates distinct stacks.** Same frames but
   different `interactive_basename` or `root_object_name` → distinct
   entries.
5. **NULL vs empty prefix.** Verify `interactive_basename == NULL`
   yields no prefix segment (matches HEAD's `interactive_name[0] != '\0'`
   check).
6. **Resize across load-factor boundary.** Insert enough unique stacks
   to force at least one resize; verify all entries survive with
   correct counts. Use a stack count just above the initial-table
   threshold and one well past it.
7. **Many resizes.** Insert thousands of unique stacks (e.g. 10_000)
   with distinct synthetic names; verify lookup of every one returns
   the right count. Catches accumulated probe-chain errors.
8. **Hash collision determinism.** Synthesise inputs known to collide
   (or force a tiny table size) and verify probe finds the right entry.
9. **Empty frame list.** `num_frames == 0` — define the policy
   (probably "no-op" or "single-line entry with prefix only") and
   assert it.
10. **Truncated marker.** A sample whose first frame is `<truncated>`
    aggregates with other truncated samples sharing the same suffix.
11. **Output ordering.** The write step does not have to sort, but the
    test pins whatever ordering the implementation chooses so changes
    are intentional.
12. **Memory hygiene (ASan-driven).** `agg_new` → many inserts →
    `agg_free` leaks nothing. `agg_new` → no inserts → `agg_free`
    leaks nothing. `agg_new` → insert → `agg_free` before write leaks
    nothing.

Assertions are hand-rolled (`#define CHECK(...)` macro printing
file:line on failure and exiting non-zero). No external framework — the
module is small enough and the build needs to stay zero-dependency.

### Acceptance criteria for Phase B

- All Phase A criteria still hold.
- `make -C test test-agg` passes cleanly under ASan+UBSan.
- Output of a representative profiling session is byte-for-byte
  equivalent to Phase A's output (modulo entry ordering if the new
  emit order differs — pin whichever order is chosen).
- Memory used during a long profiling session is dominated by unique
  stacks, not total samples. Sanity check: a 5-minute session at 100Hz
  on a busy mud uses roughly the same memory as a 30-second session,
  assuming the unique-stack set is similar.

## Sequencing

A and B are independent commits and can be reviewed separately. Land A
first; verify a baseline profile is unchanged; then land B with its
test harness.
