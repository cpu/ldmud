/*---------------------------------------------------------------------------
 * LPC Profiler - sampling profiler for generating flame graphs.
 *
 *---------------------------------------------------------------------------
 * A SIGVTALRM handler captures the LPC call stack into a SPSC producer
 * ring. The backend loop drains the ring into an aggregator (see
 * lpc_profile_agg.c) that counts samples per unique stack. On stop, the
 * aggregator writes collapsed-stacks format suitable for flamegraph.pl.
 *
 * SIGVTALRM (not SIGPROF) is used because the driver itself uses SIGPROF
 * for slow-evaluation detection (handle_profiling_signal in interpret.c).
 * ITIMER_VIRTUAL fires on user-mode CPU only, so I/O-heavy efuns may be
 * undersampled.
 *
 * Usage from LPC:
 *   profile_start("/log/profile.collapsed", 1000);  // 1000 Hz
 *   ... run workload ...
 *   profile_stop();
 *   // flamegraph.pl profile.collapsed > profile.svg
 *---------------------------------------------------------------------------
 */

#include "driver.h"

#ifdef USE_LPC_PROFILER

#include "lpc_profiler.h"
#include "lpc_profile_agg.h"

#include <fcntl.h>
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

/* Names are copied inline so the drain never dereferences pointers into
 * string_t memory that may have been freed in the meantime. Lambda frames
 * leave func_name empty; the formatter uses lambda_id instead.
 */
typedef struct {
    char        prog_name[LPC_PROFILE_PROG_LEN];
    char        func_name[LPC_PROFILE_FUNC_LEN];
    uintptr_t   lambda_id;
} lpc_profile_frame_t;

typedef struct {
    lpc_profile_frame_t frames[LPC_PROFILE_MAX_DEPTH];
    int num_frames;
    char interactive_name[LPC_PROFILE_INTERACTIVE_LEN];
    char root_object_name[LPC_PROFILE_ROOT_OBJ_LEN];
} lpc_profile_sample_t;

/* SPSC producer ring between the signal handler (producer) and the
 * drain on the backend thread (consumer). Indices are volatile so the
 * handler and the drain agree on the publish/consume order. Capacity
 * must be a power of two so the modulo reduces to a mask.
 */
#define LPC_PROFILE_PRODUCER_RING 256
#if (LPC_PROFILE_PRODUCER_RING & (LPC_PROFILE_PRODUCER_RING - 1)) != 0
#  error "LPC_PROFILE_PRODUCER_RING must be a power of two"
#endif
#define LPC_PROFILE_PRODUCER_MASK (LPC_PROFILE_PRODUCER_RING - 1)

static lpc_profile_sample_t producer_ring[LPC_PROFILE_PRODUCER_RING];
static volatile sig_atomic_t producer_head = 0;  /* drain reads here */
static volatile sig_atomic_t producer_tail = 0;  /* handler writes here */

/* Incremented when the handler finds the producer ring full and has to
 * drop a sample. Distinct from agg_dropped, which counts allocation
 * failures inside the aggregator during drain.
 */
static volatile sig_atomic_t producer_dropped = 0;
static int agg_dropped = 0;

static volatile sig_atomic_t profiler_active = 0;

/* Allocated by lpc_profile_start, freed by lpc_profile_stop. */
static lpc_profile_agg_t *agg = NULL;

/* Counters. Written only by the handler while profiler_active; read after
 * sigaction restores the previous handler, which serializes against the
 * kernel, so no atomic access is needed.
 */
static int stat_total_signals = 0;
static int stat_skipped_no_lpc = 0;
static int stat_recorded = 0;

/* Baselines for the user-CPU rate report. Only ru_utime matters since
 * ITIMER_VIRTUAL fires on user time only.
 */
static struct timeval profile_start_time;
static struct rusage profile_start_rusage;
static int profile_start_rusage_ok = 0;

static char output_filename[MAXPATHLEN];
static int profile_sample_rate = 1000;

static struct sigaction prev_sigvtalrm_action;
static Bool prev_handler_saved = MY_FALSE;

static void write_collapsed_stacks(void);
static void lpc_profile_signal_handler(int sig);

/* Signal-safe bounded string copy. Copies up to dst_size-1 bytes and
 * always NUL-terminates.
 */
static INLINE void
safe_strncpy (char *dst, const char *src, size_t dst_size)
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
lpc_profile_signal_handler (int sig)

/* SIGVTALRM handler. Must be async-signal-safe.
 *
 * Writes one slot into the producer ring and bumps the tail. If the ring
 * is full, increments producer_dropped and returns without publishing.
 */
{
    struct control_stack *p;
    struct control_stack *stack_bottom;
    lpc_profile_sample_t *sample;
    int frame_idx;
    sig_atomic_t head, tail, next_tail;
    int depth;
    Bool truncated;

    (void)sig;

    if (!profiler_active)
        return;

    stat_total_signals++;

    tail = producer_tail;
    head = producer_head;
    next_tail = (tail + 1) & LPC_PROFILE_PRODUCER_MASK;
    if (next_tail == head)
    {
        producer_dropped++;
        return;
    }
    sample = &producer_ring[tail];
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
        stat_skipped_no_lpc++;
        return;
    }

    /* Walk back from csp to the terminator entry (prog == NULL), bounded
     * by MAX_TRACE. A walk that hits the bound without finding the
     * terminator is truncated; stack_bottom is arbitrary in that case and
     * its .ob must not be trusted for root-object attribution.
     */
    stack_bottom = csp;
    for (depth = 0; depth < MAX_TRACE && stack_bottom->prog != NULL; depth++)
    {
        stack_bottom--;
    }
    truncated = (depth == MAX_TRACE && stack_bottom->prog != NULL);
    stack_bottom++;

    /* The bottom frame is the extern_call that rooted the LPC stack
     * (heartbeat/callout/command receiver). Lightweight objects carry no
     * .name, so only T_OBJECT roots are attributed.
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

    if (truncated && frame_idx < LPC_PROFILE_MAX_DEPTH)
    {
        safe_strncpy(sample->frames[frame_idx].prog_name, "<truncated>",
                     sizeof(sample->frames[frame_idx].prog_name));
        safe_strncpy(sample->frames[frame_idx].func_name, "<truncated>",
                     sizeof(sample->frames[frame_idx].func_name));
        sample->frames[frame_idx].lambda_id = 0;
        frame_idx++;
    }

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

        /* Special-funstart handling mirrors collect_trace() in
         * interpret.c / pkg-python.c. For closure calls the real identity
         * is in p->instruction.
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
            /* Lambda closure: funstart points outside the program. Use it
             * as a session-unique id; the formatter renders <lambda:0xHEX>.
             */
            lambda_id = (uintptr_t)funstart;
            func_name = NULL;
        }
        else
        {
            function_t *header = &prog->function_headers[FUNCTION_HEADER_INDEX(funstart)];
            func_name = header->name ? get_txt(header->name) : "<unknown>";
        }

        safe_strncpy(sample->frames[frame_idx].prog_name,
                     prog_name,
                     sizeof(sample->frames[frame_idx].prog_name));
        if (lambda_id)
            sample->frames[frame_idx].func_name[0] = '\0';
        else
            safe_strncpy(sample->frames[frame_idx].func_name,
                         func_name,
                         sizeof(sample->frames[frame_idx].func_name));
        sample->frames[frame_idx].lambda_id = lambda_id;
        frame_idx++;
    }

    /* SIGVTALRM can hit mid-call-setup when every walked frame is skipped.
     * Don't publish; the kernel will deliver another signal soon.
     */
    if (frame_idx == 0)
    {
        stat_skipped_no_lpc++;
        return;
    }

    sample->num_frames = frame_idx;
    producer_tail = next_tail;
    stat_recorded++;
} /* lpc_profile_signal_handler() */

void
lpc_profile_drain (void)

/* Feed any pending producer-ring slots into the aggregator. Runs on the
 * backend thread; SIGVTALRM is not masked. The handler can interrupt
 * mid-drain and write to producer_tail's slot, but it never touches
 * slots in [head, tail), so reads here are race-free.
 */
{
    sig_atomic_t tail, head;

    if (!agg)
        return;

    tail = producer_tail;
    head = producer_head;

    while (head != tail)
    {
        lpc_profile_sample_t *src = &producer_ring[head];
        lpc_profile_agg_frame_t agg_frames[LPC_PROFILE_MAX_DEPTH];
        const char *interactive_basename = NULL;
        const char *root_object_name = NULL;
        int j;

        if (src->interactive_name[0] != '\0')
        {
            const char *last_slash = strrchr(src->interactive_name, '/');
            interactive_basename = last_slash ? last_slash + 1 : src->interactive_name;
        }
        if (src->root_object_name[0] != '\0')
            root_object_name = src->root_object_name;

        for (j = 0; j < src->num_frames; j++)
        {
            agg_frames[j].prog_name = src->frames[j].prog_name;
            agg_frames[j].func_name = src->frames[j].func_name;
            agg_frames[j].lambda_id = src->frames[j].lambda_id;
        }

        if (!lpc_profile_agg_insert(agg, interactive_basename, root_object_name,
                                    agg_frames, (size_t)src->num_frames))
        {
            agg_dropped++;
        }

        head = (head + 1) & LPC_PROFILE_PRODUCER_MASK;
    }

    /* Publish the new head only after consuming, so the handler doesn't
     * reclaim a slot we haven't finished reading.
     */
    producer_head = head;
} /* lpc_profile_drain() */

Bool
lpc_profile_start (const char *filename, int sample_rate_hz)

/* filename: relative to the mudlib directory.
 * sample_rate_hz: must be in [1, 10000].
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
        debug_message("%s LPC profiler: sample rate must be 1-10000 Hz\n",
                      time_stamp());
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

    agg = lpc_profile_agg_new();
    if (!agg)
    {
        debug_message("%s LPC profiler: failed to allocate aggregator\n",
                      time_stamp());
        return MY_FALSE;
    }

    producer_head = 0;
    producer_tail = 0;
    producer_dropped = 0;
    agg_dropped = 0;
    stat_total_signals = 0;
    stat_skipped_no_lpc = 0;
    stat_recorded = 0;
    gettimeofday(&profile_start_time, NULL);
    profile_start_rusage_ok = (getrusage(RUSAGE_SELF, &profile_start_rusage) == 0);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = lpc_profile_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGVTALRM, &sa, &prev_sigvtalrm_action) == -1)
    {
        debug_message("%s LPC profiler: failed to install signal handler: %s\n",
                      time_stamp(), strerror(errno));
        lpc_profile_agg_free(agg);
        agg = NULL;
        return MY_FALSE;
    }
    prev_handler_saved = MY_TRUE;

    interval_usec = 1000000 / sample_rate_hz;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = interval_usec;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = interval_usec;

    if (setitimer(ITIMER_VIRTUAL, &timer, NULL) == -1)
    {
        debug_message("%s LPC profiler: failed to start timer: %s\n",
                      time_stamp(), strerror(errno));
        sigaction(SIGVTALRM, &prev_sigvtalrm_action, NULL);
        prev_handler_saved = MY_FALSE;
        lpc_profile_agg_free(agg);
        agg = NULL;
        return MY_FALSE;
    }

    profiler_active = 1;
    debug_message("%s LPC profiler started: output=%s, rate=%d Hz\n",
                  time_stamp(), output_filename, sample_rate_hz);
    return MY_TRUE;
} /* lpc_profile_start() */

void
lpc_profile_stop (void)
{
    struct itimerval timer;
    struct timeval now;
    struct rusage ru;
    double wall_sec = 0.0;
    double user_cpu_sec = 0.0;
    int expected;

    if (!profiler_active)
    {
        debug_message("%s LPC profiler not active\n", time_stamp());
        return;
    }

    /* Clear the active flag before disarming the timer so any pending
     * SIGVTALRM hits the handler's early-out instead of writing a stray
     * sample.
     */
    profiler_active = 0;

    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_VIRTUAL, &timer, NULL);

    if (prev_handler_saved)
    {
        sigaction(SIGVTALRM, &prev_sigvtalrm_action, NULL);
        prev_handler_saved = MY_FALSE;
    }

    /* Final drain after the handler is uninstalled — race-free. */
    lpc_profile_drain();

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
    debug_message("%s LPC profiler: signals=%d (expected ~%d at %d Hz), "
                  "skipped_no_lpc=%d (%.1f%%), recorded=%d, "
                  "producer_ring_dropped=%d, agg_dropped=%d, unique_stacks=%zu\n",
                  time_stamp(), stat_total_signals, expected, profile_sample_rate,
                  stat_skipped_no_lpc,
                  stat_total_signals > 0
                    ? (100.0 * stat_skipped_no_lpc / stat_total_signals) : 0.0,
                  stat_recorded, (int)producer_dropped, agg_dropped,
                  lpc_profile_agg_unique_stacks(agg));

    write_collapsed_stacks();

    lpc_profile_agg_free(agg);
    agg = NULL;
} /* lpc_profile_stop() */

Bool
lpc_profile_is_active(void)
{
    return profiler_active ? MY_TRUE : MY_FALSE;
}

static Bool
write_header_line (int fd, const char *fmt, ...)

/* snprintf+write one line to fd. Returns MY_FALSE on write error, leaving
 * errno set; the body write will hit the same error and report it.
 * Truncation of the formatted line is treated as success (line dropped).
 */
{
    char buf[128];
    va_list ap;
    int n;
    ssize_t w;
    size_t left;
    const char *p;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0 || (size_t)n >= sizeof(buf))
        return MY_TRUE;

    p = buf;
    left = (size_t)n;
    while (left)
    {
        w = write(fd, p, left);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return MY_FALSE;
        }
        if (w == 0)
        {
            errno = EIO;
            return MY_FALSE;
        }
        p += w;
        left -= (size_t)w;
    }
    return MY_TRUE;
} /* write_header_line() */

static void
write_collapsed_stacks (void)

/* Output line format (one per unique stack):
 *   [interactive]{root_object};prog1:func1;prog2:func2;... count
 * Both prefixes are optional. Names are not escaped: a ';' or whitespace
 * inside one would confuse flamegraph.pl, but LPC names (paths and
 * #clone-ids) are normally safe.
 */
{
    int fd;
    ssize_t bytes;

    if (!agg)
        return;

    fd = open(output_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        debug_message("%s LPC profiler: failed to open output file %s: %s\n",
                      time_stamp(), output_filename, strerror(errno));
        return;
    }

    if (producer_dropped > 0)
        write_header_line(fd, "# dropped %d samples: producer ring full\n",
                          (int)producer_dropped);
    if (agg_dropped > 0)
        write_header_line(fd, "# dropped %d samples: aggregator allocation failure\n",
                          agg_dropped);

    bytes = lpc_profile_agg_write(agg, fd);
    if (bytes < 0)
    {
        debug_message("%s LPC profiler: write error on %s: %s\n",
                      time_stamp(), output_filename, strerror(errno));
        close(fd);
        return;
    }
    if (close(fd) < 0)
    {
        debug_message("%s LPC profiler: close error on %s: %s\n",
                      time_stamp(), output_filename, strerror(errno));
        return;
    }

    debug_message("%s LPC profiler: wrote %zu unique stacks (%zd bytes) to %s\n",
                  time_stamp(), lpc_profile_agg_unique_stacks(agg),
                  bytes, output_filename);
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
