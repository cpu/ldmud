#ifndef LPC_PROFILE_AGG_H__
#define LPC_PROFILE_AGG_H__

/*---------------------------------------------------------------------------
 * LPC Profiler — sample aggregator.
 *
 * Stores each unique formatted stack once and counts occurrences. Used by
 * lpc_profiler.c to keep profile memory proportional to the number of
 * unique stacks rather than the total number of samples.
 *
 * Standalone with respect to the rest of the driver: the only inputs are
 * plain C strings, the only outputs are bytes written to a file descriptor.
 * Unit-testable via src/test/test_lpc_profile_agg.c, which compiles this
 * module against a libc shim instead of xalloc/hashmem32.
 *---------------------------------------------------------------------------
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    const char *prog_name;
    const char *func_name;
    uintptr_t   lambda_id;  /* non-zero: lambda frame, func_name ignored */
} lpc_profile_agg_frame_t;

struct lpc_profile_agg;
typedef struct lpc_profile_agg lpc_profile_agg_t;

/* Returns NULL on OOM. */
extern lpc_profile_agg_t *lpc_profile_agg_new(void);

/* NULL is a no-op. */
extern void lpc_profile_agg_free(lpc_profile_agg_t *agg);

/* Insert one sample. interactive_basename and root_object_name may be
 * NULL or empty. frames is bottom-to-top. num_frames == 0 is a no-op.
 * A leading '/' is stripped from root_object_name and each prog_name.
 * Returns false on allocation failure.
 */
extern bool lpc_profile_agg_insert(lpc_profile_agg_t *agg,
                                   const char *interactive_basename,
                                   const char *root_object_name,
                                   const lpc_profile_agg_frame_t *frames,
                                   size_t num_frames);

/* Write entries in collapsed-stacks format to fd, one line per unique
 * stack: "<stack> <count>\n". Order is first-seen insertion order.
 * Returns total bytes written, or -1 on write() error.
 */
extern ssize_t lpc_profile_agg_write(const lpc_profile_agg_t *agg, int fd);

extern size_t lpc_profile_agg_unique_stacks(const lpc_profile_agg_t *agg);

#endif /* LPC_PROFILE_AGG_H__ */
