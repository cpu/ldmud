#ifndef LPC_PROFILE_AGG_H__
#define LPC_PROFILE_AGG_H__

/*---------------------------------------------------------------------------
 * LPC Profiler — aggregator.
 *
 * Aggregates samples produced by lpc_profiler.c. Each unique stack (the
 * concatenation of optional [interactive] / {root_object} prefixes and the
 * per-frame "prog:func" or "prog:<lambda:0xHEX>" segments) is stored once
 * in a stack arena; the open-addressed hashmap keyed on those bytes maps
 * to a sample count.
 *
 * Memory cost scales with the number of *unique* stacks rather than the
 * total number of samples, which is the property the profiler rework
 * was built to deliver.
 *
 * The module is self-contained — it takes plain C strings in and produces
 * collapsed-format bytes out. No dependency on signals, the interpreter,
 * or LDMud globals. The driver build uses xalloc/rexalloc/xfree; the test
 * harness substitutes a libc shim so the module can be exercised in
 * isolation under ASan+UBSan.
 *---------------------------------------------------------------------------
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

/* One frame as seen by the aggregator.
 *   prog_name:   program path (leading '/' is stripped at format time)
 *   func_name:   function name; ignored when lambda_id != 0
 *   lambda_id:   nonzero iff this frame is a lambda; formatted as
 *                <lambda:0xHEX> in the output
 */
typedef struct {
    const char *prog_name;
    const char *func_name;
    uintptr_t   lambda_id;
} lpc_profile_agg_frame_t;

struct lpc_profile_agg;
typedef struct lpc_profile_agg lpc_profile_agg_t;

/* Allocate and initialize a new aggregator. Returns NULL on OOM. */
extern lpc_profile_agg_t *lpc_profile_agg_new(void);

/* Free the aggregator and all its backing storage. NULL is a no-op. */
extern void lpc_profile_agg_free(lpc_profile_agg_t *agg);

/* Insert one sample.
 *
 *   interactive_basename: leaf name (no path) of the commanding player, or
 *                         NULL/"" if absent. If a slash is present the
 *                         caller is responsible for stripping it, mirroring
 *                         the legacy "last component after the last /"
 *                         behavior.
 *   root_object_name:     object that rooted the LPC stack, or NULL/"" if
 *                         absent. A leading '/' is stripped by the
 *                         aggregator.
 *   frames:               frame array, bottom-to-top.
 *   num_frames:           length of frames[]; 0 means "no LPC stack" — the
 *                         sample is dropped (no entry created).
 *
 * Returns true on success, false on allocation failure (caller treats the
 * sample as dropped).
 */
extern bool lpc_profile_agg_insert(lpc_profile_agg_t *agg,
                                   const char *interactive_basename,
                                   const char *root_object_name,
                                   const lpc_profile_agg_frame_t *frames,
                                   size_t num_frames);

/* Write all aggregated entries in collapsed-stacks format to fd. One line
 * per unique stack:  <stack> <count>\n .  Returns total bytes written, or
 * -1 on write() error (with errno set).
 *
 * Order is insertion order of first occurrence — deterministic for a given
 * input stream, but not sorted. flamegraph.pl doesn't care about ordering.
 */
extern ssize_t lpc_profile_agg_write(const lpc_profile_agg_t *agg, int fd);

/* Diagnostic: number of unique stacks currently held. */
extern size_t lpc_profile_agg_unique_stacks(const lpc_profile_agg_t *agg);

#endif /* LPC_PROFILE_AGG_H__ */
