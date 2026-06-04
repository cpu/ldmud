/*---------------------------------------------------------------------------
 * LPC Profiler - Sampling profiler for generating flame graphs
 *
 * This module implements a sampling profiler that captures LPC call stacks
 * via SIGVTALRM signals and outputs collapsed stack format suitable for
 * flame graph visualization (using tools like flamegraph.pl).
 *
 * We use ITIMER_VIRTUAL / SIGVTALRM rather than ITIMER_PROF / SIGPROF
 * because the driver itself uses SIGPROF for slow-evaluation detection
 * (handle_profiling_signal in interpret.c). 
 *
 * ITIMER_VIRTUAL counts user-mode CPU only (not kernel time), which for an LPC
 * profiler is fine since interpreter cycles are user-mode. I/O-heavy efuns may
 * be slightly undersampled.
 *
 * The profiler is async-signal-safe: the signal handler only reads existing
 * data structures and writes to pre-allocated buffers. Each sample captures:
 *   - The LPC call stack (program + function or efun/sefun/lambda name)
 *   - The object that rooted the stack (heartbeat/callout/command receiver)
 *   - current_interactive, if set (e.g. during command dispatch)
 *
 * Samples land in a fixed-size ring buffer and once full, oldest samples are
 * overwritten so the final output reflects the most recent activity.
 *
 * Usage from LPC:
 *   profile_start("/log/profile.collapsed", 1000);  // 1000 Hz sampling
 *   ... run workload ...
 *   profile_stop();
 *   // Check status: profile_is_active() returns 1 if running
 *   // Then: flamegraph.pl profile.collapsed > profile.svg
 *---------------------------------------------------------------------------
 */

#include "driver.h"

#ifdef USE_LPC_PROFILER

#include "lpc_profiler.h"

#include <signal.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <errno.h>

#include "interpret.h"
#include "exec.h"
#include "instrs.h"
#include "mstrings.h"
#include "simul_efun.h"
#include "simulate.h"
#include "object.h"
#include "main.h"

/* A single frame in a captured stack. Names are inlined (copied at sample
 * time) so the writer never dereferences pointers into string_t memory
 * that may have been freed by the time write_collapsed_stacks() runs.
 * For lambdas, func_name[0] == '\0' and lambda_id holds the funstart
 * pointer's bits; the writer formats it as <lambda:0xHEX>. We can't snprintf
 * in the signal handler, so name formatting is deferred to write-time.
 */
typedef struct {
    char        prog_name[LPC_PROFILE_PROG_LEN];
    char        func_name[LPC_PROFILE_FUNC_LEN];
    uintptr_t   lambda_id;  /* Nonzero iff this frame is a lambda */
} lpc_profile_frame_t;

/* A single stack sample. Only slots the handler advanced past are live;
 * num_frames > 0 implies a recorded LPC stack. Empty-string buffers (first
 * byte '\0') mean "absent" for the optional prefixes.
 */
typedef struct {
    lpc_profile_frame_t frames[LPC_PROFILE_MAX_DEPTH];
    int num_frames;
    char interactive_name[LPC_PROFILE_INTERACTIVE_LEN];
                                    /* current_interactive->name basename,
                                       or empty. Only set during command
                                       dispatch. */
    char root_object_name[LPC_PROFILE_ROOT_OBJ_LEN];
                                    /* Bottom LPC frame's object name, or
                                       empty for lightweight objects / no
                                       object / truncated walks. */
} lpc_profile_sample_t;

/* Ring buffer of samples - pre-allocated for async-signal safety */
static lpc_profile_sample_t samples[LPC_PROFILE_MAX_SAMPLES];

/* Write index for new samples.
 * Always advances modulo LPC_PROFILE_MAX_SAMPLES; old samples are overwritten.
 */
static volatile sig_atomic_t sample_write_idx = 0;

/* Count of samples ever written, saturated at LPC_PROFILE_MAX_SAMPLES.
 * Once this reaches the cap the ring has wrapped and the oldest live sample
 * is at sample_write_idx; below the cap, live samples are [0, sample_count).
 * The saturation is what keeps the wrap detection sound across long sessions.
 */
static volatile sig_atomic_t sample_count = 0;

/* Profiler state */
static volatile sig_atomic_t profiler_active = 0;

/* Diagnostic counters, printed at lpc_profile_stop(). Only read after
 * sigaction has uninstalled the handler, which is a synchronization point
 * with the kernel, so we don't require volatile here.
 *   stat_total_signals:  every SIGVTALRM delivery while active
 *   stat_skipped_no_lpc: signals that arrived with no LPC frame on the stack
 *   stat_recorded:       samples actually written into the ring (may wrap)
 */
static int stat_total_signals = 0;
static int stat_skipped_no_lpc = 0;
static int stat_recorded = 0;

/* Wall-clock and user-CPU baselines, for samples/sec reporting. We only
 * track ru_utime since ITIMER_VIRTUAL fires on user-mode time, so the
 * "expected signals" math must compare against user time alone.
 */
static struct timeval profile_start_time;
static struct rusage profile_start_rusage;
static int profile_start_rusage_ok = 0;

/* Output filename (full path, mudlib + caller-supplied path) */
static char output_filename[MAXPATHLEN];

/* Sample rate in Hz */
static int profile_sample_rate = 1000;

/* Previous SIGVTALRM handler (to restore when stopping) */
static struct sigaction prev_sigvtalrm_action;
static Bool prev_handler_saved = MY_FALSE;

static void write_collapsed_stacks(void);
static void lpc_profile_signal_handler(int sig);
static Bool append_segment(char **p, size_t *remaining, const char *fmt, ...);

/* Signal-safe bounded string copy. Copies up to dst_size-1 bytes from src
 * into dst and always NUL-terminates. Used in the signal handler in place of
 * pointer-borrowing so the writer never dereferences string_t memory that
 * may have been freed since sampling.
 */
static INLINE void
safe_strncpy(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    if (dst_size == 0)
        return;
    if (src != NULL)
    {
        while (i < dst_size - 1 && src[i] != '\0')
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
} /* safe_strncpy() */

static void
lpc_profile_signal_handler(int sig)
/* Signal handler for SIGVTALRM. Called at sample_rate_hz frequency.
 * This function must be async-signal-safe!
 */
{
    struct control_stack *p;
    struct control_stack *stack_bottom;
    lpc_profile_sample_t *sample;
    int frame_idx;
    int write_idx;
    int depth;
    Bool truncated;

    (void)sig;

    if (!profiler_active)
        return;

    stat_total_signals++;

    /* Claim the next slot. SIGVTALRM is masked while this handler runs
     * so the load+store of sample_write_idx below has no other writer to race
     * with. Once the ring fills, we overwrite the oldest sample.
     */
    write_idx = sample_write_idx;
    sample = &samples[write_idx];
    sample->num_frames = 0;
    sample->interactive_name[0] = '\0';
    sample->root_object_name[0] = '\0';
    frame_idx = 0;

    if (current_interactive && current_interactive->name)
    {
        safe_strncpy(sample->interactive_name,
                     get_txt(current_interactive->name),
                     sizeof(sample->interactive_name));
    }

    if (!current_prog || !csp)
    {
        /* No active program, not executing LPC code */
        stat_skipped_no_lpc++;
        return;
    }

    /* Find the bottom of the control stack by walking backwards from csp
     * until we hit the terminator entry (prog == NULL), bounded by MAX_TRACE.
     * If we hit the bound without finding the terminator, the stack is
     * deeper than MAX_TRACE; in that case stack_bottom is arbitrary and we
     * must not trust its .ob for root-object attribution.
     */
    stack_bottom = csp;
    for (depth = 0; depth < MAX_TRACE && stack_bottom->prog != NULL; depth++)
    {
        stack_bottom--;
    }
    truncated = (depth == MAX_TRACE && stack_bottom->prog != NULL);
    stack_bottom++;

    /* Capture the object that rooted this LPC stack (the receiver of the
     * apply/heartbeat/call_out/command that got us into LPC). The bottom
     * frame is always an extern_call, so its .ob is meaningful. Lightweight
     * objects have no .name field, so we only attribute T_OBJECT roots.
     * Skipped on truncated walks since stack_bottom isn't really the bottom.
     */
    if (!truncated
     && stack_bottom->ob.type == T_OBJECT
     && stack_bottom->ob.u.ob
     && stack_bottom->ob.u.ob->name)
    {
        safe_strncpy(sample->root_object_name,
                     get_txt(stack_bottom->ob.u.ob->name),
                     sizeof(sample->root_object_name));
    }

    /* On truncation, prepend a synthetic <truncated> frame so the flame
     * graph clearly shows these samples don't share a real root with
     * non-truncated ones.
     */
    if (truncated && frame_idx < LPC_PROFILE_MAX_DEPTH)
    {
        safe_strncpy(sample->frames[frame_idx].prog_name, "<truncated>",
                     sizeof(sample->frames[frame_idx].prog_name));
        safe_strncpy(sample->frames[frame_idx].func_name, "<truncated>",
                     sizeof(sample->frames[frame_idx].func_name));
        sample->frames[frame_idx].lambda_id = 0;
        frame_idx++;
    }

    /* Walk the control stack from bottom to top (oldest to newest call) */
    for (p = stack_bottom; p <= csp && frame_idx < LPC_PROFILE_MAX_DEPTH; p++)
    {
        program_t *prog;
        bytecode_p funstart;
        const char *prog_name = NULL;
        const char *func_name = NULL;
        uintptr_t lambda_id = 0;

        if (p == csp)
        {
            prog = current_prog;
            funstart = csp->funstart;
        }
        else
        {
            prog = p[1].prog;
            funstart = p->funstart;
        }

        if (!prog || !funstart)
            continue;

        prog_name = prog->name ? get_txt(prog->name) : "<unknown>";

        /* Handle special function markers. The funstart magic values mark
         * synthetic frames pushed for closure calls; the real identity of
         * the call is in p->instruction (an opcode for efun closures, or an
         * index into simul_efun_table for sefun closures).
         *
         * Mirrors collect_trace() in interpret.c and pkg-python.c, which is
         * the authoritative stringification used by error traces.
         */
        if (funstart == SIMUL_EFUN_FUNSTART)
        {
            int idx = p->instruction;
            if (idx >= 0 && idx < (int)SEFUN_TABLE_SIZE
             && simul_efun_table[idx].function.name)
            {
                func_name = get_txt(simul_efun_table[idx].function.name);
            }
            else
            {
                func_name = "<simul_efun>";
            }
        }
        else if (funstart == EFUN_FUNSTART)
        {
            const char *iname = instrs[p->instruction].name;
            func_name = iname ? iname : "<efun>";
        }
#ifdef USE_PYTHON
        else if (funstart == PYTHON_EFUN_FUNSTART)
        {
            /* closure_python_efun_to_string allocates; not signal-safe. */
            func_name = "<python_efun>";
        }
#endif
        else if (funstart < prog->program || funstart > PROGRAM_END(*prog))
        {
            /* For a Lambda closure funstart points outside program. We can't
             * recover a name easily, but the funstart address uniquely
             * identifies the lambda within this profiling session; the writer
             * will format it as <lambda:0xHEX>. We avoid that formatting here
             * to remain signal-safe.
             */
            lambda_id = (uintptr_t)funstart;
            func_name = NULL;
        }
        else
        {
            /* Normal LPC function */
            function_t *header = &prog->function_headers[FUNCTION_HEADER_INDEX(funstart)];
            func_name = header->name ? get_txt(header->name) : "<unknown>";
        }

        safe_strncpy(sample->frames[frame_idx].prog_name,
                     prog_name,
                     sizeof(sample->frames[frame_idx].prog_name));
        /* Lambda frames have no func name: leave func_name empty so the
         * writer takes the <lambda:0x...> branch via lambda_id.
         */
        if (lambda_id)
            sample->frames[frame_idx].func_name[0] = '\0';
        else
            safe_strncpy(sample->frames[frame_idx].func_name,
                         func_name,
                         sizeof(sample->frames[frame_idx].func_name));
        sample->frames[frame_idx].lambda_id = lambda_id;
        frame_idx++;
    }

    /* If every frame in the walk was skipped (e.g. SIGVTALRM hit during
     * call setup when csp->funstart is still NULL), we have no real sample
     * to commit. Treat it like the no-LPC bailout: don't advance, don't
     * count as recorded.
     */
    if (frame_idx == 0)
    {
        stat_skipped_no_lpc++;
        return;
    }

    sample->num_frames = frame_idx;

    /* Commit the sample: advance write index (mod buffer size) and bump the
     * saturated count. The count saturating at the cap is what tells the
     * writer "we have wrapped, so the oldest live sample is at write_idx."
     */
    sample_write_idx = (write_idx + 1) % LPC_PROFILE_MAX_SAMPLES;
    if (sample_count < LPC_PROFILE_MAX_SAMPLES)
        sample_count++;
    stat_recorded++;
} /* lpc_profile_signal_handler() */

Bool
lpc_profile_start(const char *filename, int sample_rate_hz)
/* Start the profiler.
 * filename: path to output file (relative to mudlib directory)
 * sample_rate_hz: sampling frequency (e.g., 1000 = 1000 samples/sec = 1ms)
 * Returns: true on success
 */
{
    struct sigaction sa;
    struct itimerval timer;
    int interval_usec;

    if (profiler_active)
    {
        debug_message("%s LPC profiler already active\n", time_stamp());
        return MY_FALSE;
    }

    if (!filename)
    {
        debug_message("%s LPC profiler: invalid filename\n", time_stamp());
        return MY_FALSE;
    }

    if (sample_rate_hz <= 0 || sample_rate_hz > 10000)
    {
        debug_message("%s LPC profiler: sample rate must be 1-10000 Hz\n", time_stamp());
        return MY_FALSE;
    }

    if (!mud_lib)
    {
        debug_message("%s LPC profiler: no mud_lib directory configured.\n",
                      time_stamp());
        return MY_FALSE;
    }

    snprintf(output_filename, sizeof(output_filename), "%s%s", mud_lib, filename);
    output_filename[sizeof(output_filename) - 1] = '\0';
    profile_sample_rate = sample_rate_hz;

    /* Clear the sample buffer */
    memset(samples, 0, sizeof(samples));
    sample_write_idx = 0;
    sample_count = 0;
    stat_total_signals = 0;
    stat_skipped_no_lpc = 0;
    stat_recorded = 0;
    gettimeofday(&profile_start_time, NULL);
    profile_start_rusage_ok = (getrusage(RUSAGE_SELF, &profile_start_rusage) == 0);

    /* Install our SIGVTALRM handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = lpc_profile_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGVTALRM, &sa, &prev_sigvtalrm_action) == -1)
    {
        debug_message("%s LPC profiler: failed to install signal handler: %s\n",
                      time_stamp(), strerror(errno));
        return MY_FALSE;
    }
    prev_handler_saved = MY_TRUE;

    /* Calculate interval in microseconds */
    interval_usec = 1000000 / sample_rate_hz;

    /* Start the profiling timer */
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = interval_usec;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = interval_usec;

    if (setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1)
    {
        debug_message("%s LPC profiler: failed to start timer: %s\n",
                      time_stamp(), strerror(errno));
        /* Restore previous handler */
        sigaction(SIGVTALRM, &prev_sigvtalrm_action, NULL);
        prev_handler_saved = MY_FALSE;
        return MY_FALSE;
    }

    profiler_active = 1;
    debug_message("%s LPC profiler started: output=%s, rate=%d Hz\n",
                  time_stamp(), output_filename, sample_rate_hz);

    return MY_TRUE;
} /* lpc_profile_start() */

void
lpc_profile_stop(void)
/* Stop the profiler and write the output file. */
{
    struct itimerval timer;

    if (!profiler_active)
    {
        debug_message("%s LPC profiler not active\n", time_stamp());
        return;
    }

    /* Order matters: clear the active flag first so any signal already
     * pending (queued by the kernel before we disarm the timer) hits the
     * early-out in the handler instead of writing a stray sample.
     */
    profiler_active = 0;

    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_VIRTUAL, &timer, NULL);

    /* Restore previous signal handler */
    if (prev_handler_saved)
    {
        sigaction(SIGVTALRM, &prev_sigvtalrm_action, NULL);
        prev_handler_saved = MY_FALSE;
    }

    {
        struct timeval now;
        struct rusage ru;
        double wall_sec = 0.0;
        double user_cpu_sec = 0.0;
        int signals = stat_total_signals;
        int skipped = stat_skipped_no_lpc;
        int recorded = stat_recorded;
        int expected;

        /* Subtract in integer form before converting to double, so we don't
         * lose precision to large absolute timestamps. We only use ru_utime
         * (not ru_stime) because ITIMER_VIRTUAL fires on user time only
         */
        gettimeofday(&now, NULL);
        wall_sec = (now.tv_sec - profile_start_time.tv_sec)
                 + (now.tv_usec - profile_start_time.tv_usec) / 1.0e6;

        if (profile_start_rusage_ok && getrusage(RUSAGE_SELF, &ru) == 0)
        {
            long sec  = ru.ru_utime.tv_sec  - profile_start_rusage.ru_utime.tv_sec;
            long usec = ru.ru_utime.tv_usec - profile_start_rusage.ru_utime.tv_usec;
            user_cpu_sec = sec + usec / 1.0e6;
        }

        expected = (int)(user_cpu_sec * profile_sample_rate);

        debug_message("%s LPC profiler stopped: wall=%.2fs, "
                      "user CPU during profile=%.2fs (%.1f%% busy)\n",
                      time_stamp(), wall_sec, user_cpu_sec,
                      wall_sec > 0 ? (100.0 * user_cpu_sec / wall_sec) : 0.0);
        debug_message("%s LPC profiler: signals=%d (expected ~%d at %d Hz "
                      "for that user CPU time), skipped_no_lpc=%d "
                      "(%.1f%% of signals), recorded=%d\n",
                      time_stamp(), signals, expected, profile_sample_rate,
                      skipped,
                      signals > 0 ? (100.0 * skipped / signals) : 0.0,
                      recorded);
    }

    write_collapsed_stacks();
} /* lpc_profile_stop() */

Bool
lpc_profile_is_active(void)
{
    return profiler_active ? MY_TRUE : MY_FALSE;
}

static Bool
append_segment(char **p, size_t *remaining, const char *fmt, ...)
/* snprintf wrapper that advances (*p, *remaining) on success.
 *
 * Returns MY_TRUE if the formatted text fit (and was committed) and
 * MY_FALSE if it would have overflowed (in which case the buffer state
 * is left untouched. snprintf may have written a truncated string into
 * the buffer, but since we don't advance p, the next call overwrites it).
 */
{
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(*p, *remaining, fmt, ap);
    va_end(ap);

    if (len < 0 || (size_t)len >= *remaining)
        return MY_FALSE;

    *p += len;
    *remaining -= len;
    return MY_TRUE;
} /* append_segment() */

static void
write_collapsed_stacks(void)
/* Process all samples and write collapsed stack format to output file.
 *
 * Output line format (for flamegraph.pl):
 *   [interactive]{root_object};prog1:func1;prog2:func2;... count
 *
 * Both prefixes are optional. We emit one line per sample with count=1 and
 * let flamegraph.pl aggregate identical stacks. In the future it might be
 * nice to do some of that aggregation here.
 *
 * Note: program/object/function names are not escaped. Any ';' or whitespace
 * inside a name would confuse flamegraph.pl's parser. LPC names are normally
 * safe (paths and #clone-ids), so we don't sanitize.
 */
{
    FILE *fp;
    sig_atomic_t i, total_samples, start_idx;
    int j;
    int written_samples = 0;
    char stack_buf[LPC_PROFILE_MAX_DEPTH * LPC_PROFILE_FRAME_LEN];

    fp = fopen(output_filename, "w");
    if (!fp)
    {
        debug_message("%s LPC profiler: failed to open output file %s: %s\n",
                      time_stamp(), output_filename, strerror(errno));
        return;
    }

    /* If we wrapped, the oldest live sample is at sample_write_idx and there
     * are LPC_PROFILE_MAX_SAMPLES of them; otherwise samples [0, sample_count)
     * are live in order.
     */
    if (sample_count >= LPC_PROFILE_MAX_SAMPLES)
    {
        total_samples = LPC_PROFILE_MAX_SAMPLES;
        start_idx = sample_write_idx;
    }
    else
    {
        total_samples = sample_count;
        start_idx = 0;
    }

    for (i = 0; i < total_samples; i++)
    {
        sig_atomic_t idx = (start_idx + i) % LPC_PROFILE_MAX_SAMPLES;
        lpc_profile_sample_t *sample = &samples[idx];
        char *p = stack_buf;
        size_t remaining = sizeof(stack_buf);
        Bool first_segment = MY_TRUE;

        if (sample->num_frames == 0)
            continue;

        /* Optional prefixes:
         *   [interactive]  - commanding player (only set during command dispatch)
         *   {root_object}  - object whose extern_call rooted the LPC stack
         *                    (absent for lightweight objects and other edge cases)
         */
        if (sample->interactive_name[0] != '\0')
        {
            const char *name = sample->interactive_name;
            const char *last_slash = strrchr(name, '/');
            if (last_slash)
                name = last_slash + 1;
            if (append_segment(&p, &remaining, "[%s]", name))
                first_segment = MY_FALSE;
        }

        if (sample->root_object_name[0] != '\0')
        {
            const char *root = sample->root_object_name;
            if (root[0] == '/')
                root++;
            if (append_segment(&p, &remaining, "{%s}", root))
                first_segment = MY_FALSE;
        }

        /* Frames: bottom to top, separated by ';'. The separator goes before
         * every segment after the first, regardless of which prefixes (if
         * any) preceded it.
         */
        for (j = 0; j < sample->num_frames && remaining > 1; j++)
        {
            const char *prog = sample->frames[j].prog_name;
            const char *func = sample->frames[j].func_name;
            uintptr_t lambda_id = sample->frames[j].lambda_id;
            Bool ok;

            if (prog[0] == '/')
                prog++;

            if (!first_segment)
            {
                if (remaining < 2)
                    break;
                *p++ = ';';
                remaining--;
            }

            if (lambda_id)
            {
                /* Lambda frame: name is the funstart address so distinct
                 * lambdas get distinct flame graph buckets.
                 */
                ok = append_segment(&p, &remaining,
                                    "%s:<lambda:0x%" PRIxPTR ">",
                                    prog, lambda_id);
            }
            else
            {
                ok = append_segment(&p, &remaining, "%s:%s", prog, func);
            }
            if (!ok)
                break;
            first_segment = MY_FALSE;
        }

        /* snprintf already null-terminated the buffer; no extra '\0' needed. */

        fprintf(fp, "%s 1\n", stack_buf);
        written_samples++;
    }

    fclose(fp);

    debug_message("%s LPC profiler: wrote %d samples to %s\n",
                  time_stamp(), written_samples, output_filename);

    if (written_samples != (int)total_samples)
    {
        /* Should be impossible in the current model: every advanced slot
         * has num_frames > 0. If this fires, some invariant changed.
         */
        debug_message("%s LPC profiler: WARNING %d samples in buffer but only "
                      "%d written (%d empty/skipped) — handler/writer "
                      "invariants may have drifted\n",
                      time_stamp(), (int)total_samples, written_samples,
                      (int)total_samples - written_samples);
    }
} /* write_collapsed_stacks() */

svalue_t *
f_profile_start(svalue_t *sp)
/* EFUN profile_start()
 *
 *   int profile_start(string filename, int sample_rate_hz)
 *
 * Start the sampling profiler. Samples will be collected at
 * sample_rate_hz frequency and written to filename in collapsed
 * stack format when profile_stop() is called.
 *
 * The filename is relative to the mudlib directory.
 *
 * Returns 1 on success, 0 on failure.
 */
{
    string_t *filename;
    p_int sample_rate;
    Bool result;

    sample_rate = sp->u.number;
    filename = (sp-1)->u.str;

    result = lpc_profile_start(get_txt(filename), (int)sample_rate);

    free_svalue(sp);
    free_svalue(sp-1);

    put_number(sp-1, result ? 1 : 0);
    return sp - 1;
} /* f_profile_start() */

svalue_t *
f_profile_stop(svalue_t *sp)
/* EFUN profile_stop()
 *
 *   void profile_stop()
 *
 * Stop the profiler and write collected samples to the output file.
 */
{
    lpc_profile_stop();
    return sp;
} /* f_profile_stop() */

svalue_t *
f_profile_is_active(svalue_t *sp)
/* EFUN profile_is_active()
 *
 *   int profile_is_active()
 *
 * Returns 1 if the profiler is currently active, 0 otherwise.
 */
{
    push_number(sp, lpc_profile_is_active() ? 1 : 0);
    return sp;
} /* f_profile_is_active() */

#endif /* USE_LPC_PROFILER */
