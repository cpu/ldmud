#ifndef LPC_PROFILER_H__
#define LPC_PROFILER_H__

#include "driver.h"

#ifdef USE_LPC_PROFILER

#include "svalue.h"

#define LPC_PROFILE_MAX_DEPTH       64

/* Per-sample inline name buffers. Names are copied here at sample time so
 * the drain never dereferences a string_t that may have been freed in the
 * interim (e.g. when an object is destructed between sampling and drain).
 */
#define LPC_PROFILE_PROG_LEN        96
#define LPC_PROFILE_FUNC_LEN        64
#define LPC_PROFILE_INTERACTIVE_LEN 64
#define LPC_PROFILE_ROOT_OBJ_LEN   128

extern Bool lpc_profile_start(const char *filename, int sample_rate_hz);
extern void lpc_profile_stop(void);
extern Bool lpc_profile_is_active(void);

/* Move pending producer-ring slots into the aggregator. Must run on the
 * backend thread, not from the signal handler.
 */
extern void lpc_profile_drain(void);

extern svalue_t *f_profile_start(svalue_t *sp);
extern svalue_t *f_profile_stop(svalue_t *sp);
extern svalue_t *f_profile_is_active(svalue_t *sp);

#endif /* USE_LPC_PROFILER */

#endif /* LPC_PROFILER_H__ */
