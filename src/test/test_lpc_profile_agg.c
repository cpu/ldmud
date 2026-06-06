/*---------------------------------------------------------------------------
 * Standalone unit tests for src/lpc_profile_agg.c.
 *
 * Builds with LPC_PROFILE_AGG_STANDALONE_TEST defined, which makes the
 * module compile against libc malloc/realloc/free and an inline FNV-1a
 * hash function rather than the driver's xalloc/hashmem32. No dependency
 * on the rest of the driver — these tests can be built and run in
 * isolation under ASan+UBSan.
 *
 * Build via the test/Makefile: `make -C src/test test-agg`.
 *
 * The harness intentionally avoids any external test framework. The
 * module is small, the build needs to stay zero-dependency, and a
 * hand-rolled CHECK macro is plenty.
 *---------------------------------------------------------------------------
 */

#define _DEFAULT_SOURCE   1   /* for mkstemp on -std=c11 */
#define LPC_PROFILE_AGG_STANDALONE_TEST 1
#include "../lpc_profile_agg.c"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*-------------------------------------------------------------------------*/
/* Harness                                                                 */
/*-------------------------------------------------------------------------*/

static int g_fail_count;
static int g_pass_count;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_fail_count++;                                             \
        } else {                                                        \
            g_pass_count++;                                             \
        }                                                               \
    } while (0)

#define CHECK_STREQ(actual, expected)                                   \
    do {                                                                \
        const char *_a = (actual);                                      \
        const char *_e = (expected);                                    \
        if (!_a || strcmp(_a, _e) != 0) {                               \
            fprintf(stderr, "FAIL %s:%d: expected \"%s\", got \"%s\"\n", \
                    __FILE__, __LINE__, _e, _a ? _a : "(null)");        \
            g_fail_count++;                                             \
        } else {                                                        \
            g_pass_count++;                                             \
        }                                                               \
    } while (0)

#define RUN(test)                                                       \
    do {                                                                \
        fprintf(stderr, "  %s ...\n", #test);                           \
        test();                                                         \
    } while (0)

/* Capture agg_write output into an in-memory buffer by writing to a temp
 * file and reading it back. Easier than mocking write(); also exercises
 * the real syscall path which is part of what we want to verify.
 */
static char *capture_write(lpc_profile_agg_t *agg, size_t *out_len)
{
    char tmpl[] = "/tmp/lpc_agg_test_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        perror("mkstemp");
        exit(2);
    }
    ssize_t w = lpc_profile_agg_write(agg, fd);
    if (w < 0) {
        perror("agg_write");
        close(fd);
        unlink(tmpl);
        exit(2);
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        perror("lseek");
        close(fd);
        unlink(tmpl);
        exit(2);
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat");
        close(fd);
        unlink(tmpl);
        exit(2);
    }
    char *buf = (char *)malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        unlink(tmpl);
        exit(2);
    }
    ssize_t r = read(fd, buf, (size_t)st.st_size);
    if (r < 0) {
        perror("read");
        free(buf);
        close(fd);
        unlink(tmpl);
        exit(2);
    }
    buf[r] = '\0';
    close(fd);
    unlink(tmpl);
    *out_len = (size_t)r;
    return buf;
}

/* Helper to build a simple frame array. */
static void
mk_frame(lpc_profile_agg_frame_t *f, const char *prog, const char *func, uintptr_t lambda)
{
    f->prog_name = prog;
    f->func_name = func;
    f->lambda_id = lambda;
}

/* Count the number of lines in `s`. */
static size_t
count_lines(const char *s)
{
    size_t n = 0;
    while (*s) {
        if (*s == '\n') n++;
        s++;
    }
    return n;
}

/*-------------------------------------------------------------------------*/
/* Tests                                                                   */
/*-------------------------------------------------------------------------*/

/* 1. Single sample, single frame. */
static void test_single_sample(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    CHECK(agg != NULL);

    lpc_profile_agg_frame_t f;
    mk_frame(&f, "/foo/bar", "fn", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    size_t len;
    char *out = capture_write(agg, &len);
    /* Leading '/' is stripped from prog name; expect exactly "foo/bar:fn 1\n". */
    CHECK_STREQ(out, "foo/bar:fn 1\n");
    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    free(out);
    lpc_profile_agg_free(agg);
}

/* 2. Repeat insertion increments count. */
static void test_repeat_increments_count(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    mk_frame(&f, "/foo", "g", 0);
    for (int i = 0; i < 7; i++)
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK_STREQ(out, "foo:g 7\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 3. Distinct stacks stay distinct. */
static void test_distinct_stacks(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t a, b;
    mk_frame(&a, "/foo", "a", 0);
    mk_frame(&b, "/foo", "b", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &a, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &b, 1));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 2);

    size_t len;
    char *out = capture_write(agg, &len);
    /* Insertion order: a then b. */
    CHECK_STREQ(out, "foo:a 1\nfoo:b 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 4. Prefix variation creates distinct stacks. */
static void test_prefix_variation(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    mk_frame(&f, "/foo", "g", 0);

    CHECK(lpc_profile_agg_insert(agg, "alice", NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, "bob",   NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, "/room/start", &f, 1));
    CHECK(lpc_profile_agg_insert(agg, NULL, "/room/other", &f, 1));
    /* Same prefixes as the second-to-last should *not* duplicate */
    CHECK(lpc_profile_agg_insert(agg, NULL, "/room/start", &f, 1));

    CHECK(lpc_profile_agg_unique_stacks(agg) == 4);

    size_t len;
    char *out = capture_write(agg, &len);
    /* Order: alice, bob, {room/start}=2, {room/other}.
     * A ';' separates the optional prefix segment from the frames — mirrors
     * the legacy collapsed-stacks formatter's "every segment after the
     * first gets a leading ;" rule.
     */
    CHECK_STREQ(out,
                "[alice];foo:g 1\n"
                "[bob];foo:g 1\n"
                "{room/start};foo:g 2\n"
                "{room/other};foo:g 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 5. NULL vs empty interactive prefix yields no [..] segment. */
static void test_null_empty_prefix(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    mk_frame(&f, "p", "fn", 0);

    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    CHECK(lpc_profile_agg_insert(agg, "",   NULL, &f, 1));   /* should bucket with NULL */
    CHECK(lpc_profile_agg_insert(agg, NULL, "",   &f, 1));   /* same */

    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK_STREQ(out, "p:fn 3\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 6. Resize across load-factor boundary.
 *
 * Initial capacity is 64; load factor is 0.5, so the table grows when the
 * 33rd unique entry is about to be inserted. Inserting 40 distinct stacks
 * forces at least one resize and lets us verify all entries survive with
 * correct counts.
 */
static void test_resize_across_load_boundary(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    char prog[32];
    lpc_profile_agg_frame_t f;

    for (int i = 0; i < 40; i++) {
        snprintf(prog, sizeof(prog), "p%d", i);
        mk_frame(&f, prog, "fn", 0);
        /* Insert each (i+1) times so counts are distinguishable. */
        for (int k = 0; k <= i; k++)
            CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    }
    CHECK(lpc_profile_agg_unique_stacks(agg) == 40);

    /* Spot-check a few counts by looking them up via re-insert and seeing
     * the published count get bumped. Easier: just walk the output text.
     */
    size_t len;
    char *out = capture_write(agg, &len);

    /* For every i in [0,40), verify a line "pi:fn <i+1>\n" appears. */
    for (int i = 0; i < 40; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), "p%d:fn %d\n", i, i + 1);
        CHECK(strstr(out, needle) != NULL);
    }
    CHECK(count_lines(out) == 40);

    free(out);
    lpc_profile_agg_free(agg);
}

/* 7. Many resizes. 10000 unique stacks exercises probe-chain correctness
 *    across many table growths.
 */
static void test_many_resizes(void)
{
    enum { N = 10000 };
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    char prog[32];
    lpc_profile_agg_frame_t f;

    for (int i = 0; i < N; i++) {
        snprintf(prog, sizeof(prog), "/path/p%05d", i);
        mk_frame(&f, prog, "f", 0);
        /* Insert twice; second call must find the existing entry. */
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    }
    CHECK(lpc_profile_agg_unique_stacks(agg) == N);

    /* Verify every key resolves to count 2 by re-inserting once and then
     * inspecting the output for count 3.
     */
    for (int i = 0; i < N; i++) {
        snprintf(prog, sizeof(prog), "/path/p%05d", i);
        mk_frame(&f, prog, "f", 0);
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    }

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK(count_lines(out) == N);
    /* Spot-check first, middle, last. */
    CHECK(strstr(out, "path/p00000:f 3\n") != NULL);
    CHECK(strstr(out, "path/p05000:f 3\n") != NULL);
    CHECK(strstr(out, "path/p09999:f 3\n") != NULL);
    free(out);
    lpc_profile_agg_free(agg);
}

/* 8. Hash collision determinism. We can't force collisions in FNV-1a
 *    easily, but if we keep table capacity small relative to the inserted
 *    count, linear probing will be heavily exercised. This test inserts
 *    many entries and asserts every one is retrievable.
 *
 *    (The "force a tiny table" mode the plan mentions would require
 *    parameterizing the initial capacity. Adding that just for tests
 *    isn't worth it — many_resizes covers the same property.)
 */
static void test_collision_determinism(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();

    /* Build pairs that share a multi-frame prefix but differ in the
     * trailing frame, so insertion order matters and the hash for each
     * full key depends on the whole stack.
     */
    lpc_profile_agg_frame_t frames[3];
    mk_frame(&frames[0], "shared", "root", 0);
    mk_frame(&frames[1], "shared", "middle", 0);

    for (int i = 0; i < 200; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "leaf%d", i);
        mk_frame(&frames[2], "p", buf, 0);
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 3));
        CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 3));
    }
    CHECK(lpc_profile_agg_unique_stacks(agg) == 200);

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK(count_lines(out) == 200);
    /* Each entry's count should be 2. */
    for (int i = 0; i < 200; i++) {
        char needle[64];
        snprintf(needle, sizeof(needle), ";p:leaf%d 2\n", i);
        CHECK(strstr(out, needle) != NULL);
    }
    free(out);
    lpc_profile_agg_free(agg);
}

/* 9. Empty frame list. Policy: no-op, no entry created. */
static void test_empty_frames(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    CHECK(lpc_profile_agg_insert(agg, "alice", "/room", NULL, 0));
    CHECK(lpc_profile_agg_unique_stacks(agg) == 0);

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK(len == 0);
    CHECK_STREQ(out, "");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 10. Truncated marker. Samples whose first frame is "<truncated>"
 *     aggregate with others sharing the same suffix.
 */
static void test_truncated_marker(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t frames[2];
    mk_frame(&frames[0], "<truncated>", "<truncated>", 0);
    mk_frame(&frames[1], "p", "fn", 0);

    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 2));
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 2));
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, frames, 2));

    CHECK(lpc_profile_agg_unique_stacks(agg) == 1);

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK_STREQ(out, "<truncated>:<truncated>;p:fn 3\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 11. Output ordering pin. Insertion order should be preserved. */
static void test_output_ordering(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;

    mk_frame(&f, "z", "z", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    mk_frame(&f, "a", "a", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));
    mk_frame(&f, "m", "m", 0);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK_STREQ(out, "z:z 1\na:a 1\nm:m 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 12. Lambda frame formatting. */
static void test_lambda_format(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    mk_frame(&f, "/foo", NULL, 0xdeadbeef);
    CHECK(lpc_profile_agg_insert(agg, NULL, NULL, &f, 1));

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK_STREQ(out, "foo:<lambda:0xdeadbeef> 1\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/* 13. Memory hygiene under ASan: lifecycle variants. */
static void test_memory_hygiene(void)
{
    /* new -> free, no inserts. */
    lpc_profile_agg_t *a = lpc_profile_agg_new();
    CHECK(a != NULL);
    lpc_profile_agg_free(a);

    /* new -> many inserts -> free, no write. */
    a = lpc_profile_agg_new();
    lpc_profile_agg_frame_t f;
    for (int i = 0; i < 500; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "p%d", i);
        mk_frame(&f, buf, "f", 0);
        CHECK(lpc_profile_agg_insert(a, NULL, NULL, &f, 1));
    }
    lpc_profile_agg_free(a);

    /* free(NULL) is a no-op. */
    lpc_profile_agg_free(NULL);
}

/* 14. Multi-frame stack with prefixes. */
static void test_multi_frame_with_prefixes(void)
{
    lpc_profile_agg_t *agg = lpc_profile_agg_new();
    lpc_profile_agg_frame_t frames[3];
    mk_frame(&frames[0], "/room/start", "init", 0);
    mk_frame(&frames[1], "/lib/std/player", "look", 0);
    mk_frame(&frames[2], "/obj/desc", "describe", 0);

    CHECK(lpc_profile_agg_insert(agg, "alice", "/room/start", frames, 3));
    CHECK(lpc_profile_agg_insert(agg, "alice", "/room/start", frames, 3));

    size_t len;
    char *out = capture_write(agg, &len);
    CHECK_STREQ(out,
                "[alice]{room/start};room/start:init;"
                "lib/std/player:look;obj/desc:describe 2\n");
    free(out);
    lpc_profile_agg_free(agg);
}

/*-------------------------------------------------------------------------*/

int main(void)
{
    RUN(test_single_sample);
    RUN(test_repeat_increments_count);
    RUN(test_distinct_stacks);
    RUN(test_prefix_variation);
    RUN(test_null_empty_prefix);
    RUN(test_resize_across_load_boundary);
    RUN(test_many_resizes);
    RUN(test_collision_determinism);
    RUN(test_empty_frames);
    RUN(test_truncated_marker);
    RUN(test_output_ordering);
    RUN(test_lambda_format);
    RUN(test_memory_hygiene);
    RUN(test_multi_frame_with_prefixes);

    fprintf(stderr, "\n%d passed, %d failed\n", g_pass_count, g_fail_count);
    return g_fail_count == 0 ? 0 : 1;
}
