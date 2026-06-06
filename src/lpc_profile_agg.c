/*---------------------------------------------------------------------------
 * LPC Profiler — aggregator implementation.
 *
 * Stack arena: an append-only byte buffer that holds every unique stack's
 * concatenated formatted bytes back to back. Records are addressed by an
 * (offset, length) pair held in the hashmap entries; pointers into the
 * arena are invalidated by every grow, which is why we never hand out raw
 * pointers and never compare across reallocs.
 *
 * Hashmap: open-addressed, power-of-two capacity, linear probing, load
 * factor 0.5. Entries point back into the arena via offset+length and hold
 * a precomputed 32-bit hash so resize doesn't have to re-hash. An empty
 * slot has length == 0; legitimate entries always have length >= 1 (the
 * format guarantees at least one frame segment per inserted sample, since
 * num_frames == 0 is rejected up front).
 *
 * Insertion order is preserved via a flat array `order[]` of arena offsets
 * in first-seen order; emit() walks this array so output is deterministic
 * for a given input.
 *---------------------------------------------------------------------------
 */

#ifdef LPC_PROFILE_AGG_STANDALONE_TEST
/* The standalone test harness compiles this file directly; it provides
 * its own allocator shim and stubs out the driver dependency.
 */
#  include <stdlib.h>
#  include <string.h>
#  define AGG_ALLOC(size)        malloc(size)
#  define AGG_REALLOC(ptr, size) realloc((ptr), (size))
#  define AGG_FREE(ptr)          free(ptr)
/* Reproduce hashmem32 inline so the harness doesn't drag in driver headers. */
#  include <stdint.h>
static inline uint32_t agg_hashmem32(const void *key, size_t len)
{
    /* FNV-1a 32-bit. Identical-quality-good-enough for in-process hash
     * tables and easy to inline. The driver build uses the project's
     * hashmem32 (which is more capable), but the test harness only needs
     * a deterministic mixing function.
     */
    const unsigned char *p = (const unsigned char *)key;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}
#  define HASHMEM32(key, len) agg_hashmem32((key), (len))
typedef uint32_t hash32_t;
#  define Bool int
#  define MY_TRUE 1
#  define MY_FALSE 0
#else
#  include "driver.h"
#  include "xalloc.h"
#  include "hash.h"
#  include <string.h>
#  define AGG_ALLOC(size)        xalloc(size)
#  define AGG_REALLOC(ptr, size) rexalloc((ptr), (size))
#  define AGG_FREE(ptr)          xfree(ptr)
#  define HASHMEM32(key, len)    hashmem32((key), (len))
#endif

#include "lpc_profile_agg.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#define AGG_INITIAL_CAPACITY 64       /* must be a power of two */
#define AGG_INITIAL_ARENA    4096     /* bytes */
#define AGG_INITIAL_ORDER    64       /* entry slots in insertion-order array */
#define AGG_LOAD_NUM         1        /* load factor = NUM/DEN = 0.5 */
#define AGG_LOAD_DEN         2

typedef struct {
    hash32_t hash;       /* precomputed hash of arena[offset .. offset+length) */
    uint32_t offset;     /* byte offset into arena; valid only when length > 0 */
    uint32_t length;     /* stack length in bytes; 0 marks an empty slot */
    uint64_t count;
} agg_entry_t;

struct lpc_profile_agg {
    /* Hashmap */
    agg_entry_t *entries;
    size_t       capacity;        /* power of two */
    size_t       size;            /* live (non-empty) entries */

    /* Stack arena */
    unsigned char *arena;
    size_t         arena_size;    /* bytes used */
    size_t         arena_cap;     /* allocated capacity */

    /* Insertion-order index. One entry per unique stack, in first-seen
     * order. Stores enough to locate the live entry in the hashmap
     * (offset+length+hash) so write() can fetch the current count without
     * a linear scan. Length matches `size`.
     *
     * We can't store agg_entry_t pointers here because table_grow()
     * reallocates entries[].
     */
    struct {
        uint32_t offset;
        uint32_t length;
        hash32_t hash;
    } *order;
    size_t order_cap;
};

/*-------------------------------------------------------------------------*/
/* Arena helpers                                                           */
/*-------------------------------------------------------------------------*/

/* Ensure the arena has room for `need` more bytes; grow by doubling. */
static bool
arena_reserve(lpc_profile_agg_t *agg, size_t need)
{
    size_t new_cap;
    unsigned char *new_arena;

    if (agg->arena_size + need <= agg->arena_cap)
        return true;

    new_cap = agg->arena_cap ? agg->arena_cap : AGG_INITIAL_ARENA;
    while (new_cap < agg->arena_size + need)
    {
        if (new_cap > SIZE_MAX / 2)
            return false;
        new_cap *= 2;
    }

    new_arena = (unsigned char *)AGG_REALLOC(agg->arena, new_cap);
    if (!new_arena)
        return false;
    agg->arena = new_arena;
    agg->arena_cap = new_cap;
    return true;
}

/* Append n bytes to the arena and return the offset of the first byte. */
static bool
arena_append(lpc_profile_agg_t *agg, const void *bytes, size_t n, uint32_t *out_offset)
{
    if (agg->arena_size > UINT32_MAX - n)
        return false;
    if (!arena_reserve(agg, n))
        return false;
    *out_offset = (uint32_t)agg->arena_size;
    memcpy(agg->arena + agg->arena_size, bytes, n);
    agg->arena_size += n;
    return true;
}

/* Append a single formatted segment to the arena. Used while building a
 * sample's stack bytes in-place. printf-style; returns false on overflow
 * (very-very-long names) or OOM.
 */
static bool
arena_appendf(lpc_profile_agg_t *agg, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    int n;
    uint32_t off;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp))
        return false;
    return arena_append(agg, tmp, (size_t)n, &off);  /* off unused */
}

/* Roll the arena back to a saved size — used when building a sample then
 * discovering it's a duplicate, so we don't bloat the arena with copies.
 */
static void
arena_truncate(lpc_profile_agg_t *agg, size_t saved_size)
{
    agg->arena_size = saved_size;
}

/*-------------------------------------------------------------------------*/
/* Hashmap helpers                                                         */
/*-------------------------------------------------------------------------*/

/* Find the slot for (hash, key_bytes, key_len). Returns the index of the
 * matching entry if present, or the index of the first empty slot if not.
 * Caller distinguishes by inspecting entries[idx].length.
 */
static size_t
table_probe(const lpc_profile_agg_t *agg,
            hash32_t hash, const unsigned char *key, size_t key_len)
{
    size_t mask = agg->capacity - 1;
    size_t idx = (size_t)hash & mask;

    for (;;)
    {
        agg_entry_t *e = &agg->entries[idx];
        if (e->length == 0)
            return idx;
        if (e->hash == hash
         && e->length == key_len
         && memcmp(agg->arena + e->offset, key, key_len) == 0)
            return idx;
        idx = (idx + 1) & mask;
    }
}

/* Grow the table to 2x and rehash all live entries. */
static bool
table_grow(lpc_profile_agg_t *agg)
{
    size_t old_cap = agg->capacity;
    size_t new_cap = old_cap * 2;
    agg_entry_t *new_entries;
    size_t mask;

    if (new_cap < old_cap)
        return false;  /* overflow */

    new_entries = (agg_entry_t *)AGG_ALLOC(new_cap * sizeof(*new_entries));
    if (!new_entries)
        return false;
    memset(new_entries, 0, new_cap * sizeof(*new_entries));

    mask = new_cap - 1;
    for (size_t i = 0; i < old_cap; i++)
    {
        agg_entry_t *e = &agg->entries[i];
        if (e->length == 0)
            continue;
        size_t idx = (size_t)e->hash & mask;
        while (new_entries[idx].length != 0)
            idx = (idx + 1) & mask;
        new_entries[idx] = *e;
    }

    AGG_FREE(agg->entries);
    agg->entries = new_entries;
    agg->capacity = new_cap;
    return true;
}

/* True iff inserting one more entry would put us at or above the load
 * factor (so we should grow before inserting).
 */
static bool
table_at_load(const lpc_profile_agg_t *agg)
{
    return (agg->size + 1) * AGG_LOAD_DEN >= agg->capacity * AGG_LOAD_NUM;
}

/* Push the locator for a newly inserted entry onto the insertion-order
 * array. The locator stores enough state to re-find the entry in the
 * hashmap (offset + length + hash); the count is fetched live at write
 * time so a duplicate-insert bump is reflected in the output even though
 * the order array is touched only on first insert.
 */
static bool
order_push(lpc_profile_agg_t *agg, uint32_t offset, uint32_t length, hash32_t hash)
{
    if (agg->size + 1 > agg->order_cap)
    {
        size_t new_cap = agg->order_cap ? agg->order_cap * 2 : AGG_INITIAL_ORDER;
        void *new_order = AGG_REALLOC(agg->order, new_cap * sizeof(*agg->order));
        if (!new_order)
            return false;
        agg->order = new_order;
        agg->order_cap = new_cap;
    }
    agg->order[agg->size].offset = offset;
    agg->order[agg->size].length = length;
    agg->order[agg->size].hash = hash;
    return true;
}

/*-------------------------------------------------------------------------*/
/* Public API                                                              */
/*-------------------------------------------------------------------------*/

lpc_profile_agg_t *
lpc_profile_agg_new(void)
{
    lpc_profile_agg_t *agg = (lpc_profile_agg_t *)AGG_ALLOC(sizeof(*agg));
    if (!agg)
        return NULL;
    memset(agg, 0, sizeof(*agg));

    agg->capacity = AGG_INITIAL_CAPACITY;
    agg->entries = (agg_entry_t *)AGG_ALLOC(agg->capacity * sizeof(*agg->entries));
    if (!agg->entries)
    {
        AGG_FREE(agg);
        return NULL;
    }
    memset(agg->entries, 0, agg->capacity * sizeof(*agg->entries));

    /* arena and order grow lazily on first insert. */
    return agg;
}

void
lpc_profile_agg_free(lpc_profile_agg_t *agg)
{
    if (!agg)
        return;
    AGG_FREE(agg->entries);
    AGG_FREE(agg->arena);
    AGG_FREE(agg->order);
    AGG_FREE(agg);
}

bool
lpc_profile_agg_insert(lpc_profile_agg_t *agg,
                       const char *interactive_basename,
                       const char *root_object_name,
                       const lpc_profile_agg_frame_t *frames,
                       size_t num_frames)
{
    size_t saved_arena_size;
    uint32_t stack_offset;
    size_t stack_len;
    bool first_segment;
    hash32_t hash;
    size_t idx;
    agg_entry_t *entry;

    if (num_frames == 0)
        return true;  /* policy: no-op, not an error */

    /* Build the formatted stack bytes by appending into the arena. If we
     * later find this stack is a duplicate, we truncate the arena back to
     * saved_arena_size so the bytes don't accumulate.
     */
    saved_arena_size = agg->arena_size;
    stack_offset = (uint32_t)saved_arena_size;
    first_segment = true;

    if (interactive_basename && interactive_basename[0] != '\0')
    {
        if (!arena_appendf(agg, "[%s]", interactive_basename))
            goto fail_rollback;
        first_segment = false;
    }

    if (root_object_name && root_object_name[0] != '\0')
    {
        const char *root = root_object_name;
        if (root[0] == '/')
            root++;
        if (!arena_appendf(agg, "{%s}", root))
            goto fail_rollback;
        first_segment = false;
    }

    for (size_t i = 0; i < num_frames; i++)
    {
        const char *prog = frames[i].prog_name ? frames[i].prog_name : "<unknown>";
        const char *func = frames[i].func_name ? frames[i].func_name : "<unknown>";
        uintptr_t lambda_id = frames[i].lambda_id;

        if (prog[0] == '/')
            prog++;

        if (!first_segment)
        {
            uint32_t sep_off;
            if (!arena_append(agg, ";", 1, &sep_off))
                goto fail_rollback;
        }

        if (lambda_id)
        {
            if (!arena_appendf(agg, "%s:<lambda:0x%" PRIxPTR ">", prog, lambda_id))
                goto fail_rollback;
        }
        else
        {
            if (!arena_appendf(agg, "%s:%s", prog, func))
                goto fail_rollback;
        }
        first_segment = false;
    }

    stack_len = agg->arena_size - saved_arena_size;
    if (stack_len == 0)
    {
        /* All segments were empty (prefixes absent, all frames invalid).
         * Drop, treat as no-op.
         */
        arena_truncate(agg, saved_arena_size);
        return true;
    }
    if (stack_len > UINT32_MAX)
        goto fail_rollback;

    /* Look up; the bytes we just appended are the key. */
    hash = HASHMEM32(agg->arena + stack_offset, stack_len);
    idx = table_probe(agg, hash, agg->arena + stack_offset, stack_len);
    entry = &agg->entries[idx];

    if (entry->length != 0)
    {
        /* Existing entry — bump its count and roll back the arena. */
        entry->count++;
        arena_truncate(agg, saved_arena_size);
        return true;
    }

    /* New entry. Grow if inserting would push us past the load factor. */
    if (table_at_load(agg))
    {
        if (!table_grow(agg))
            goto fail_rollback;
        /* Re-probe in the new table. */
        idx = table_probe(agg, hash, agg->arena + stack_offset, stack_len);
        entry = &agg->entries[idx];
    }

    if (!order_push(agg, stack_offset, (uint32_t)stack_len, hash))
        goto fail_rollback;

    entry->hash = hash;
    entry->offset = stack_offset;
    entry->length = (uint32_t)stack_len;
    entry->count = 1;
    agg->size++;
    return true;

fail_rollback:
    arena_truncate(agg, saved_arena_size);
    return false;
}

size_t
lpc_profile_agg_unique_stacks(const lpc_profile_agg_t *agg)
{
    return agg ? agg->size : 0;
}

/* write(2) that retries on EINTR and handles short writes. */
static ssize_t
write_full(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    size_t left = n;
    while (left)
    {
        ssize_t w = write(fd, p, left);
        if (w < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)n;
}

ssize_t
lpc_profile_agg_write(const lpc_profile_agg_t *agg, int fd)
{
    ssize_t total = 0;

    if (!agg)
        return 0;

    for (size_t i = 0; i < agg->size; i++)
    {
        const unsigned char *bytes = agg->arena + agg->order[i].offset;
        uint32_t length = agg->order[i].length;
        hash32_t hash = agg->order[i].hash;
        size_t idx = table_probe(agg, hash, bytes, length);
        agg_entry_t *e = &agg->entries[idx];
        char line_buf[64];
        ssize_t w;
        int n;

        if (e->length == 0)
            continue;  /* should be unreachable since locators come from real inserts */

        w = write_full(fd, bytes, length);
        if (w < 0)
            return -1;
        total += w;

        n = snprintf(line_buf, sizeof(line_buf),
                     " %" PRIu64 "\n", e->count);
        if (n < 0 || (size_t)n >= sizeof(line_buf))
            return -1;
        w = write_full(fd, line_buf, (size_t)n);
        if (w < 0)
            return -1;
        total += w;
    }

    return total;
}
