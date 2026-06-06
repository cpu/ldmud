/*---------------------------------------------------------------------------
 * Unit tests for src/lpc_profile_agg.c.
 *
 * Builds standalone (no driver headers) via LPC_PROFILE_AGG_STANDALONE_TEST,
 * which swaps xalloc/hashmem32 for libc + FNV-1a. Run as:
 *
 *     make -C src/test test-agg
 *
 * ASan+UBSan are on by default so use-after-free, off-by-one resize, and
 * uninitialised-memory bugs surface immediately.
 *---------------------------------------------------------------------------
 */

#define _DEFAULT_SOURCE 1   /* mkstemp under -std=c11 */
#define LPC_PROFILE_AGG_STANDALONE_TEST 1
#include "../lpc_profile_agg.c"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail_count;
static int g_pass_count;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail_count++;                                              \
        } else g_pass_count++;                                           \
    } while (0)

#define CHECK_STREQ(actual, expected)                                    \
    do {                                                                 \
        const char *_a = (actual);                                       \
        const char *_e = (expected);                                     \
        if (!_a || strcmp(_a, _e) != 0) {                                \
            fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                    __FILE__, __LINE__, _e, _a ? _a : "(null)");         \
            g_fail_count++;                                              \
        } else g_pass_count++;                                           \
    } while (0)

#define RUN(test)                                                        \
    do {                                                                 \
        fprintf(stderr, "  %s ...\n", #test);                            \
        test();                                                          \
    } while (0)

/* Drain the aggregator into a freshly-allocated, NUL-terminated buffer
 * via a temp file. Caller frees.
 */
static char *
capture_write (lpc_profile_agg_t *agg)
{
    char tmpl[] = "/tmp/lpc_agg_test_XXXXXX";
    int fd;
    struct stat st;
    char *buf;
    ssize_t r;

    fd = mkstemp(tmpl);
    if (fd < 0) { perror("mkstemp"); exit(2); }
    unlink(tmpl);  /* unlink-on-create: the fd keeps the file alive */

    if (lpc_profile_agg_write(agg, fd) < 0) { perror("agg_write"); exit(2); }
    if (lseek(fd, 0, SEEK_SET) < 0)        { perror("lseek"); exit(2); }
    if (fstat(fd, &st) < 0)                { perror("fstat"); exit(2); }

    buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) exit(2);
    r = read(fd, buf, (size_t)st.st_size);
    if (r < 0) { perror("read"); exit(2); }
    buf[r] = '\0';
    close(fd);
    return buf;
} /* capture_write() */

static void
mk_frame (lpc_profile_agg_frame_t *f,
          const char *prog, const char *func, uintptr_t lambda)
{
    f->prog_name = prog;
    f->func_name = func;
    f->lambda_id = lambda;
} /* mk_frame() */

static size_t
count_lines (const char *s)
{
    size_t n = 0;
    while (*s) { if (*s == '\n') n++; s++; }
    return n;
} /* count_lines() */

/*-------------------------------------------------------------------------*/
/* Tests */

static void test_single_sample (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char *out;

    CHECK(agg != NULL);
    mk_frame(&f, "/foo/bar", "fn", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    out = capture_write(agg);
    CHECK_STREQ(out, "foo/bar:fn 1\n");  /* leading '/' stripped */
    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    free(out);
    lpc_profile_agg_free(agg);
}

static void test_repeat_increments_count (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char *out;

    mk_frame(&f, "/foo", "g", 0);
    for (int i = 0; i < 7; i++)
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    out = capture_write(agg);
    CHECK_STREQ(out, "foo:g 7\n");
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_distinct_stacks (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t a, b;
    char *out;

    mk_frame(&a, "/foo", "a", 0);
    mk_frame(&b, "/foo", "b", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &a, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &b, 1));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 2);

    out = capture_write(agg);
    CHECK_STREQ(out, "foo:a 1\nfoo:b 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* The ';' between the prefix segment and the first frame mirrors the
 * formatter's "any segment after the first gets a leading ;" rule.
 */
static void test_prefix_variation (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char *out;

    mk_frame(&f, "/foo", "g", 0);
    CHECK(lpc_profile_agg_insert(agg, "alice", NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, "bob",   NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, "/room/start", &f, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, "/room/other", &f, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, "/room/start", &f, 1));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 4);

    out = capture_write(agg);
    CHECK_STREQ(out,
                "[alice];foo:g 1\n"
                "[bob];foo:g 1\n"
                "{room/start};foo:g 2\n"
                "{room/other};foo:g 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_null_empty_prefix (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char *out;

    mk_frame(&f, "p", "fn", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, "",   NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, "",   &f, 1));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    out = capture_write(agg);
    CHECK_STREQ(out, "p:fn 3\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* Initial cap 64, load factor 0.5 → grow on the 33rd unique insert. */
static void test_resize_across_load_boundary (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char prog[32];
    char *out;

    for (int i = 0; i < 40; i++) {
        snprintf(prog, sizeof(prog), "p%d", i);
        mk_frame(&f, prog, "fn", 0);
        for (int k = 0; k <= i; k++)  /* count = i+1 */
            CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    }
    CHECK(lpc_profile_agg_unique_stacks(agg) == 40);

    out = capture_write(agg);
    for (int i = 0; i < 40; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "p%d:fn %d\n", i, i + 1);
        CHECK(strstr(out, needle) != NULL);
    }
    CHECK(count_lines(out) == 40);
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_many_resizes (void)
{
    enum { N = 10000 };
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char prog[32];
    char *out;

    for (int i = 0; i < N; i++) {
        snprintf(prog, sizeof(prog), "/path/p%05d", i);
        mk_frame(&f, prog, "f", 0);
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    }
    CHECK(lpc_profile_agg_unique_stacks(agg) == N);

    for (int i = 0; i < N; i++) {
        snprintf(prog, sizeof(prog), "/path/p%05d", i);
        mk_frame(&f, prog, "f", 0);
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    }

    out = capture_write(agg);
    CHECK(count_lines(out) == N);
    CHECK(strstr(out, "path/p00000:f 3\n") != NULL);
    CHECK(strstr(out, "path/p05000:f 3\n") != NULL);
    CHECK(strstr(out, "path/p09999:f 3\n") != NULL);
    free(out);
    lpc_profile_agg_free(agg);
}

/* Stresses probe chains by inserting many same-prefix stacks. */
static void test_probe_chains (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t frames[3];
    char *out;

    mk_frame(&frames[0], "shared", "root",   0);
    mk_frame(&frames[1], "shared", "middle", 0);
    for (int i = 0; i < 200; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "leaf%d", i);
        mk_frame(&frames[2], "p", buf, 0);
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 3));
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 3));
    }
    CHECK(lpc_profile_agg_unique_stacks(agg) == 200);

    out = capture_write(agg);
    CHECK(count_lines(out) == 200);
    for (int i = 0; i < 200; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), ";p:leaf%d 2\n", i);
        CHECK(strstr(out, needle) != NULL);
    }
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_empty_frames (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    char *out;

    CHECK(lpc_profile_agg_insert(agg, "alice", "/room", NULL, 0));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 0);

    out = capture_write(agg);
    CHECK_STREQ(out, "");
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_truncated_marker (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t frames[2];
    char *out;

    mk_frame(&frames[0], "<truncated>", "<truncated>", 0);
    mk_frame(&frames[1], "p", "fn", 0);
    for (int i = 0; i < 3; i++)
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 2));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    out = capture_write(agg);
    CHECK_STREQ(out, "<truncated>:<truncated>;p:fn 3\n");
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_output_ordering (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char *out;

    mk_frame(&f, "z", "z", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    mk_frame(&f, "a", "a", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    mk_frame(&f, "m", "m", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    out = capture_write(agg);
    CHECK_STREQ(out, "z:z 1\na:a 1\nm:m 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_lambda_format (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    char *out;

    mk_frame(&f, "/foo", NULL, 0xdeadbeef);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    out = capture_write(agg);
    CHECK_STREQ(out, "foo:<lambda:0xdeadbeef> 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

static void test_memory_hygiene (void)
{
    lpc_profile_agg_t *a;
    lpc_profile_agg_frame_t f;

    /* new -> free, no inserts. */
    a = lpc_profile_agg_new();
    CHECK(a != NULL);
    lpc_profile_agg_free(a);

    /* new -> many inserts -> free, no write. */
    a = lpc_profile_agg_new();
    for (int i = 0; i < 500; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "p%d", i);
        mk_frame(&f, buf, "f", 0);
        CHECK(lpc_profile_agg_insert(a, NULL, NULL, &f, 1));
    }
    lpc_profile_agg_free(a);

    lpc_profile_agg_free(NULL);
}

static void test_multi_frame_with_prefixes (void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t frames[3];
    char *out;

    mk_frame(&frames[0], "/room/start",     "init",     0);
    mk_frame(&frames[1], "/lib/std/player", "look",     0);
    mk_frame(&frames[2], "/obj/desc",       "describe", 0);

    CHECK(lpc_profile_agg_insert(agg, "alice", "/room/start", frames, 3));
    CHECK(lpc_profile_agg_insert(agg, "alice", "/room/start", frames, 3));

    out = capture_write(agg);
    CHECK_STREQ(out,
                "[alice]{room/start};room/start:init;"
                "lib/std/player:look;obj/desc:describe 2\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/*-------------------------------------------------------------------------*/

int
main (void)
{
    RUN(test_single_sample);
    RUN(test_repeat_increments_count);
    RUN(test_distinct_stacks);
    RUN(test_prefix_variation);
    RUN(test_null_empty_prefix);
    RUN(test_resize_across_load_boundary);
    RUN(test_many_resizes);
    RUN(test_probe_chains);
    RUN(test_empty_frames);
    RUN(test_truncated_marker);
    RUN(test_output_ordering);
    RUN(test_lambda_format);
    RUN(test_memory_hygiene);
    RUN(test_multi_frame_with_prefixes);

    fprintf(stderr, "\n%d passed, %d failed\n", g_pass_count, g_fail_count);
    return g_fail_count == 0 ? 0 : 1;
}
