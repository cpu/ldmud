#ifndef LPC_PROFILER_H__
#define LPC_PROFILER_H__

#include "driver.h"

#ifdef USE_LPC_PROFILER

#include "svalue.h"

/* Maximum stack depth we capture per sample */
#define LPC_PROFILE_MAX_DEPTH 64

/* Maximum samples we store (ring buffer size)
 * Note: LPC_PROFILE_MAX_SAMPLES is defined in config.h via configure
 * (--with-profile-samples=N, default 100000)
 */

/* Maximum length of a single frame string (prog:func) */
#define LPC_PROFILE_FRAME_LEN 256

extern Bool lpc_profile_start(const char *filename, int sample_rate_hz);
extern void lpc_profile_stop(void);
extern Bool lpc_profile_is_active(void);

extern svalue_t *f_profile_start(svalue_t *sp);
extern svalue_t *f_profile_stop(svalue_t *sp);
extern svalue_t *f_profile_is_active(svalue_t *sp);

#endif /* USE_LPC_PROFILER */

#endif /* LPC_PROFILER_H__ */
