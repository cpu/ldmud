/*---------------------------------------------------------------------------
 * LPC Profiler — sample aggregator.
 *
 *---------------------------------------------------------------------------
 * Unique stacks are stored as concatenated formatted bytes in an
 * append-only arena; an open-addressed hashmap (power-of-two, linear
 * probing, load factor 0.5) keys arena (offset, length) windows to sample
 * counts. Insertion order is preserved in a flat locator array so output
 * is deterministic.
 *
 * The arena may move on grow, so we never hand out pointers across calls
 * and never compare entries by pointer.
 *---------------------------------------------------------------------------
 */

#ifdef LPC_PROFILE_AGG_STANDALONE_TEST
/* Standalone test harness: replace driver primitives with libc + FNV-1a. */
#  include <stdlib.h>
#  include <string.h>
#  include <stdint.h>
#  define AGG_ALLOC(size)        malloc(size)
#  define AGG_REALLOC(ptr, size) realloc((ptr), (size))
#  define AGG_FREE(ptr)          free(ptr)
typedef uint32_t hash32_t;
static inline hash32_t agg_hashmem32(const void *key, size_t len)
{
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

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>

#define AGG_INITIAL_CAPACITY 64       /* power of two */
#define AGG_INITIAL_ARENA    4096
#define AGG_INITIAL_ORDER    64

typedef struct {
    hash32_t hash;
    uint32_t offset;     /* into arena; valid when length > 0 */
    uint32_t length;     /* 0 marks an empty slot */
    uint64_t count;
} agg_entry_t;

/* Insertion-order locator: enough state to re-probe the hashmap for the
 * current count without retaining pointers across table_grow().
 */
typedef struct {
    uint32_t offset;
    uint32_t length;
    hash32_t hash;
} agg_locator_t;

struct lpc_profile_agg {
    agg_entry_t   *entries;
    size_t         capacity;        /* power of two */
    size_t         size;            /* live entries */

    unsigned char *arena;
    size_t         arena_size;
    size_t         arena_cap;

    agg_locator_t *order;
    size_t         order_cap;
};

/*-------------------------------------------------------------------------*/
static bool
arena_reserve (lpc_profile_agg_t *agg, size_t need)

/* Ensure the arena has room for `need` more bytes; grow by doubling.
 */
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
} /* arena_reserve() */

/*-------------------------------------------------------------------------*/
static bool
arena_append (lpc_profile_agg_t *agg, const void *bytes, size_t n)

/* Append n bytes to the arena. Fails if arena_size would exceed
 * UINT32_MAX (offsets are stored as uint32_t).
 */
{
    if (n > UINT32_MAX || agg->arena_size > UINT32_MAX - n)
        return false;
    if (!arena_reserve(agg, n))
        return false;
    memcpy(agg->arena + agg->arena_size, bytes, n);
    agg->arena_size += n;
    return true;
} /* arena_append() */

/*-------------------------------------------------------------------------*/
static bool
arena_appendf (lpc_profile_agg_t *agg, const char *fmt, ...)

/* Append printf-formatted text to the arena. Segments must fit in a
 * 512-byte scratch buffer; profiler frames are always well under that.
 */
{
    char tmp[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sizeof(tmp))
        return false;
    return arena_append(agg, tmp, (size_t)n);
} /* arena_appendf() */

/*-------------------------------------------------------------------------*/
static size_t
table_probe (const lpc_profile_agg_t *agg,
             hash32_t hash, const unsigned char *key, size_t key_len)

/* Return the slot for (hash, key). If present, the slot holds the entry;
 * otherwise it's the first empty slot. Caller distinguishes via .length.
 * Guaranteed to terminate because we never let the table fill: load
 * factor is capped at 0.5.
 */
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
} /* table_probe() */

/*-------------------------------------------------------------------------*/
static bool
table_grow (lpc_profile_agg_t *agg)

/* Double the table and rehash. */
{
    size_t old_cap = agg->capacity;
    size_t new_cap = old_cap * 2;
    agg_entry_t *new_entries;
    size_t mask;

    if (new_cap < old_cap)
        return false;

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
} /* table_grow() */

/*-------------------------------------------------------------------------*/
static bool
order_push (lpc_profile_agg_t *agg,
            uint32_t offset, uint32_t length, hash32_t hash)

/* Append a locator for a new entry. Called once per unique stack. */
{
    if (agg->size + 1 > agg->order_cap)
    {
        size_t new_cap = agg->order_cap ? agg->order_cap * 2 : AGG_INITIAL_ORDER;
        agg_locator_t *new_order = (agg_locator_t *)
            AGG_REALLOC(agg->order, new_cap * sizeof(*new_order));
        if (!new_order)
            return false;
        agg->order = new_order;
        agg->order_cap = new_cap;
    }
    agg->order[agg->size].offset = offset;
    agg->order[agg->size].length = length;
    agg->order[agg->size].hash = hash;
    return true;
} /* order_push() */

/*-------------------------------------------------------------------------*/
lpc_profile_agg_t *
lpc_profile_agg_new (void)
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
    return agg;
} /* lpc_profile_agg_new() */

/*-------------------------------------------------------------------------*/
void
lpc_profile_agg_free (lpc_profile_agg_t *agg)
{
    if (!agg)
        return;
    AGG_FREE(agg->entries);
    AGG_FREE(agg->arena);
    AGG_FREE(agg->order);
    AGG_FREE(agg);
} /* lpc_profile_agg_free() */

/*-------------------------------------------------------------------------*/
bool
lpc_profile_agg_insert (lpc_profile_agg_t *agg,
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
        return true;

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
            if (!arena_append(agg, ";", 1))
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
        /* Prefixes absent and every frame skipped — nothing to record. */
        return true;
    }

    hash = HASHMEM32(agg->arena + stack_offset, stack_len);
    idx = table_probe(agg, hash, agg->arena + stack_offset, stack_len);
    entry = &agg->entries[idx];

    if (entry->length != 0)
    {
        entry->count++;
        agg->arena_size = saved_arena_size;  /* duplicate; drop the bytes */
        return true;
    }

    if ((agg->size + 1) * 2 >= agg->capacity)
    {
        if (!table_grow(agg))
            goto fail_rollback;
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
    agg->arena_size = saved_arena_size;
    return false;
} /* lpc_profile_agg_insert() */

/*-------------------------------------------------------------------------*/
size_t
lpc_profile_agg_unique_stacks (const lpc_profile_agg_t *agg)
{
    return agg ? agg->size : 0;
} /* lpc_profile_agg_unique_stacks() */

/*-------------------------------------------------------------------------*/
static ssize_t
write_full (int fd, const void *buf, size_t n)

/* write(2) loop that retries EINTR and handles short writes. Treats a
 * zero-byte return as an error to avoid spinning forever on an
 * unwritable fd.
 */
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
        if (w == 0)
        {
            errno = EIO;
            return -1;
        }
        p += w;
        left -= (size_t)w;
    }
    return (ssize_t)n;
} /* write_full() */

/*-------------------------------------------------------------------------*/
ssize_t
lpc_profile_agg_write (const lpc_profile_agg_t *agg, int fd)
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
        char line_buf[32];
        ssize_t w;
        int n;

        w = write_full(fd, bytes, length);
        if (w < 0)
            return -1;
        total += w;

        n = snprintf(line_buf, sizeof(line_buf), " %" PRIu64 "\n", e->count);
        if (n < 0 || (size_t)n >= sizeof(line_buf))
        {
            errno = EOVERFLOW;
            return -1;
        }
        w = write_full(fd, line_buf, (size_t)n);
        if (w < 0)
            return -1;
        total += w;
    }

    return total;
} /* lpc_profile_agg_write() */
