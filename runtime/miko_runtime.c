/* miko_runtime.c - C runtime services for the miko -> LLVM IR backend.
 *
 * The IR emitted by compiler/codegen_ll.miko declares these functions and
 * calls them; keeping print / string / file / argv logic in C keeps the IR
 * generator small. Compiled together with the generated .ll (or .obj) by
 * clang, or ahead of time and linked.
 */

/* _GNU_SOURCE is required for pthread_getattr_np on Linux, and it must come
 * before any header. It only widens what the C library exposes. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
/* GetCurrentThreadStackLimits is documented from Windows 8 on. miko targets
 * Windows 10 and later, so asking for that API level is accurate rather than
 * optimistic. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602
#endif
#include <windows.h>
#include <direct.h>
static void miko_mkdir(const char* path) {
    _mkdir(path);
}
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <pthread.h>
static void miko_mkdir(const char* path) {
    mkdir(path, 0777);
}
#endif

static int miko_argc_storage = 0;

/* ---- string representation ------------------------------------------------
 *
 * A miko string is still a `char*`: NUL terminated, usable with printf("%s"),
 * memcpy, strcmp and every other C string function. What changed is that every
 * string the runtime hands back or that the compiler emits carries the byte
 * length in a hidden header immediately before the first character:
 *
 *     [ size_t length ][ 'h' 'e' 'l' 'l' 'o' 0 ]
 *                        ^ the char* points here
 *
 * Why: `len(s)` used to be strlen(s), and so did char_at, substring and every
 * concat operand. A loop shaped
 *
 *     while i < len(s) { total = total + char_at(s, i); i = i + 1; }
 *
 * therefore re-walked the whole string on every character - O(n^2) for a scan
 * that touches each byte once. Measured on a 20 KB string scanned 20 times:
 * 278 ms, against 2 ms once the length is a header read.
 *
 * The header keeps the C ABI, which a {length, data} struct would not: the IR
 * still passes `ptr` around, the runtime can still hand a miko string to
 * printf, and string literals stay readable in the emitted IR.
 *
 * Invariant: every pointer that reaches miko_str_len() came from
 * miko_str_alloc(), miko_str_adopt(), or a literal emitted with the same header
 * by the code generator. Interior pointers into a string must never be passed
 * to it as if they were strings.
 */
typedef struct {
    size_t length;
    size_t capacity;   /* bytes available after the header, excluding the NUL */
} MikoStringHeader;

#define MIKO_HEADER_SIZE (sizeof(MikoStringHeader))

/* ---- garbage collection ---------------------------------------------------
 *
 * Every string used to be allocated and never freed: `free()` was called zero
 * times in this file. A loop building two million small strings reached 99.8 MB
 * of peak RSS for about 16 MB of live data, growing linearly and without bound,
 * so no long-running miko program was viable.
 *
 * What is collected: strings, and only strings. Structs live in `alloca` slots on
 * the stack (the backend emits alloca for every aggregate), so the heap holds no
 * aggregate and **no heap object contains a pointer to another heap object**. That
 * single fact is what keeps this collector small and safe: marking is shallow,
 * with no traversal, no recursion, and no chance of following a stale pointer
 * inside string data.
 *
 * How liveness is decided: conservatively. The stack is scanned word by word and
 * any word that is exactly the address of a live block marks that block. This is
 * the same approach Boehm's collector takes, and it needs no cooperation from the
 * code generator - which matters because the IR passes strings around as plain
 * `ptr`. A value that happens to look like a block address only delays a free by
 * one cycle, which is harmless; the danger in a conservative collector is the
 * opposite one, freeing something still in use, so every ambiguity here resolves
 * toward keeping.
 *
 * Registers: a pointer that lives only in a register would not be seen by a stack
 * scan, so setjmp() spills the register file onto the stack before the scan
 * begins. That is the standard trick and the reason the collector cannot miss a
 * value it is holding.
 *
 * Stack bounds: obtained from the operating system, per platform. If that ever
 * fails the collector turns itself off and the program keeps the old
 * allocate-and-never-free behaviour. Leaking is recoverable; freeing a live string
 * is not.
 */

typedef struct MikoGcBlock {
    struct MikoGcBlock* next;
    struct MikoGcBlock* prev;
    size_t bytes;
    int marked;
} MikoGcBlock;

/* The collector metadata sits *before* the string header, so the pointers the IR
 * already uses are unchanged: miko_str_header(text) is still text - header size,
 * and a string literal emitted into .data is still a valid miko string with no
 * metadata. Literals are never registered, so they are never freed. */
static MikoGcBlock miko_gc_head = { &miko_gc_head, &miko_gc_head, 0, 0 };

static char* miko_gc_stack_low = NULL;    /* exclusive: lowest address to scan */
static char* miko_gc_stack_high = NULL;   /* inclusive: highest address to scan */
static int miko_gc_ready = 0;
static int miko_gc_disabled = 0;
static size_t miko_gc_live_bytes = 0;
static size_t miko_gc_threshold = 0;
static size_t miko_gc_next_collect = 0;
static int miko_gc_stress = -1;           /* MIKO_GC_STRESS: collect every alloc */
static long long miko_gc_runs = 0;
static long long miko_gc_freed_blocks = 0;

/* ---- address set: which pointers are live block starts --------------------
 *
 * Marking asks "is this word a block start?" once per stack word, so a linked
 * list walk would be quadratic. Open addressing with linear probing gives a
 * constant-time answer; rebuilds keep the load factor under 3/4.
 */

typedef struct {
    void** keys;
    size_t capacity;   /* power of two */
    size_t used;       /* keys != NULL and != TOMBSTONE */
    size_t filled;     /* used + tombstones */
} MikoGcSet;

static void* const MIKO_GC_TOMBSTONE = (void*)(size_t)1;
static MikoGcSet miko_gc_set;

static size_t miko_gc_hash(void* key) {
    size_t value = (size_t)key;
    value ^= value >> 33;
    value *= (size_t)0xff51afd7ed558ccdULL;
    value ^= value >> 29;
    return value;
}

static void miko_gc_set_put_raw(MikoGcSet* set, void* key) {
    size_t mask = set->capacity - 1;
    size_t slot = miko_gc_hash(key) & mask;
    while (set->keys[slot] != NULL && set->keys[slot] != MIKO_GC_TOMBSTONE) {
        if (set->keys[slot] == key) {
            return;
        }
        slot = (slot + 1) & mask;
    }
    set->keys[slot] = key;
    set->used = set->used + 1;
}

static int miko_gc_set_grow(void) {
    size_t old_capacity = miko_gc_set.capacity;
    void** old_keys = miko_gc_set.keys;
    size_t new_capacity = old_capacity == 0 ? 1024 : old_capacity * 2;
    void** fresh = (void**)calloc(new_capacity, sizeof(void*));
    if (!fresh) {
        return 0;
    }
    miko_gc_set.keys = fresh;
    miko_gc_set.capacity = new_capacity;
    miko_gc_set.used = 0;
    miko_gc_set.filled = 0;
    for (size_t i = 0; i < old_capacity; i++) {
        if (old_keys[i] != NULL && old_keys[i] != MIKO_GC_TOMBSTONE) {
            miko_gc_set_put_raw(&miko_gc_set, old_keys[i]);
        }
    }
    free(old_keys);
    /* put_raw does not maintain `filled`, and `filled` is what keeps the table
     * from ever becoming completely full. Leaving it at zero after a rehash let
     * `used` climb to `capacity`; at that point linear probing had no empty slot
     * to stop at and the collector spun forever. After a rehash every entry is
     * live, so filled equals used by definition. */
    miko_gc_set.filled = miko_gc_set.used;
    return 1;
}

/* Rebuilds the address set from the block list.
 *
 * An earlier version maintained the set incrementally with tombstones, and that
 * incremental maintenance was the source of an intermittent access violation: a
 * removed entry could survive a probe chain and later authorise a write to a
 * block the heap had already taken back. The set has exactly one job - answer
 * "is this address a live block start" - and the block list already holds that
 * answer, so the set is thrown away and refilled once per collection. Live sets
 * are small next to the number of allocations, so the cost is a single pass and
 * the invariant becomes trivial: after this call the set names every live block
 * and nothing else. */
static void miko_gc_set_clear(void) {
    if (miko_gc_set.keys != NULL) {
        free(miko_gc_set.keys);
    }
    miko_gc_set.keys = NULL;
    miko_gc_set.capacity = 0;
    miko_gc_set.used = 0;
    miko_gc_set.filled = 0;
}

static void miko_gc_set_insert(void* key) {
    if (miko_gc_set.capacity == 0 || (miko_gc_set.filled + 1) * 4 >= miko_gc_set.capacity * 3) {
        if (!miko_gc_set_grow()) {
            return;
        }
    }
    size_t mask = miko_gc_set.capacity - 1;
    size_t slot = miko_gc_hash(key) & mask;
    while (miko_gc_set.keys[slot] != NULL && miko_gc_set.keys[slot] != MIKO_GC_TOMBSTONE) {
        if (miko_gc_set.keys[slot] == key) {
            return;
        }
        slot = (slot + 1) & mask;
    }
    if (miko_gc_set.keys[slot] == NULL) {
        miko_gc_set.filled = miko_gc_set.filled + 1;
    }
    miko_gc_set.keys[slot] = key;
    miko_gc_set.used = miko_gc_set.used + 1;
}

static int miko_gc_set_has(const void* key) {
    if (miko_gc_set.capacity == 0) {
        return 0;
    }
    size_t mask = miko_gc_set.capacity - 1;
    size_t slot = miko_gc_hash((void*)key) & mask;
    while (miko_gc_set.keys[slot] != NULL) {
        if (miko_gc_set.keys[slot] == key) {
            return 1;
        }
        slot = (slot + 1) & mask;
    }
    return 0;
}


/* ---- stack bounds -------------------------------------------------------- */

static int miko_gc_learn_stack_bounds(void) {
#ifdef _WIN32
    /* GetCurrentThreadStackLimits needs a higher API level than MinGW declares by
     * default, and the TEB layout is not documented. VirtualQuery is available on
     * every version and every toolchain: the stack is one reservation, so its
     * AllocationBase is the low end and walking up while the base stays the same
     * finds the high end. */
    int here = 0;
    MEMORY_BASIC_INFORMATION info;
    char* probe = (char*)&here;
    if (VirtualQuery(probe, &info, sizeof(info)) == 0) {
        return 0;
    }
    char* base = (char*)info.AllocationBase;
    if (base == NULL) {
        return 0;
    }
    char* top = base;
    char* cursor = base;
    MEMORY_BASIC_INFORMATION walk;
    /* The reservation holds three kinds of page: committed and readable (in use),
     * reserved but not committed (below the stack, never touched yet), and the
     * guard page just under the current frame, which faults on touch. Only the
     * first kind may be read, so the scanned range is the span of those pages
     * rather than the whole reservation. Skipping a region instead of stopping at
     * it matters: the bottom of the stack is usually still uncommitted, so
     * breaking there produced an empty range and silently switched the collector
     * off. */
    char* low_seen = NULL;
    char* high_seen = NULL;
    while (VirtualQuery(cursor, &walk, sizeof(walk)) != 0) {
        if ((char*)walk.AllocationBase != base) {
            break;
        }
        int readable = walk.State == MEM_COMMIT
            && (walk.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        if (readable) {
            char* region_low = (char*)walk.BaseAddress;
            char* region_high = region_low + walk.RegionSize;
            if (low_seen == NULL || region_low < low_seen) {
                low_seen = region_low;
            }
            if (high_seen == NULL || region_high > high_seen) {
                high_seen = region_high;
            }
        }
        cursor = (char*)walk.BaseAddress + walk.RegionSize;
    }
    if (high_seen != NULL) {
        top = high_seen;
    }
    if (low_seen != NULL) {
        base = low_seen;
    }
    if (base >= top) {
        return 0;
    }
    miko_gc_stack_low = base;
    miko_gc_stack_high = top;
    return 1;
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    void* top = pthread_get_stackaddr_np(self);
    size_t size = pthread_get_stacksize_np(self);
    if (top == NULL || size == 0) {
        return 0;
    }
    miko_gc_stack_high = (char*)top;
    miko_gc_stack_low = (char*)top - size;
    return 1;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) {
        return 0;
    }
    void* base = NULL;
    size_t size = 0;
    int ok = pthread_attr_getstack(&attr, &base, &size) == 0;
    pthread_attr_destroy(&attr);
    if (!ok || base == NULL || size == 0) {
        return 0;
    }
    miko_gc_stack_low = (char*)base;
    miko_gc_stack_high = (char*)base + size;
    return 1;
#else
    return 0;
#endif
}

/* Called from miko_set_args, the first thing a generated main() does, so the
 * bounds are learned before any user code can allocate. */
static void miko_gc_init(void) {
    if (miko_gc_ready) {
        return;
    }
    if (miko_gc_stack_low == NULL) {
        if (!miko_gc_learn_stack_bounds()) {
            miko_gc_disabled = 1;
            return;
        }
    }
    const char* stress = getenv("MIKO_GC_STRESS");
    if (stress != NULL && stress[0] != 0 && stress[0] != '0') {
        miko_gc_stress = 1;
    } else {
        miko_gc_stress = 0;
    }
    const char* stats = getenv("MIKO_GC_STATS");
    if (stats != NULL && stats[0] != 0 && stats[0] != '0') {
        miko_gc_ready = 1;
        miko_gc_disabled = 2;   /* report only; never free */
        return;
    }
    miko_gc_threshold = 8u * 1024u * 1024u;
    if (miko_gc_stress) {
        miko_gc_threshold = 0;
    }
    miko_gc_next_collect = miko_gc_threshold;
    miko_gc_ready = 1;
}

/* Prints the collector's counters at exit under MIKO_GC_VERBOSE=1, so a workload
 * can be measured without a profiler. */
static void miko_gc_report_at_exit(void) {
    fprintf(stderr, "[gc] runs=%lld freed=%lld live=%lldKB next=%lldKB\n",
            miko_gc_runs, miko_gc_freed_blocks,
            (long long)(miko_gc_live_bytes / 1024), (long long)(miko_gc_next_collect / 1024));
}

static void miko_gc_install_report(void) {
    const char* verbose = getenv("MIKO_GC_VERBOSE");
    if (verbose != NULL && verbose[0] != 0 && verbose[0] != '0') {
        atexit(miko_gc_report_at_exit);
    }
}

/* ---- collect ------------------------------------------------------------- */

static void miko_gc_collect(void) {
    if (miko_gc_disabled) {
        return;
    }
    jmp_buf registers;
    /* Spill the register file so a pointer held only in a register is on the
     * stack when the scan below runs. setjmp/setjmp is the portable way to do
     * it; the buffer is never longjmp'd to. */
    if (setjmp(registers) != 0) {
        return;
    }

    /* Mark: shallow, because no heap object points at another. */
    /* The committed span of the stack changes as the program runs: pages below
     * the starting point only become readable once a deeper frame touches them.
     * Bounds measured once at startup therefore described the stack as it was at
     * the first call, and re-measuring at collection time is what keeps the scan
     * covering every frame that is live right now. */
    miko_gc_learn_stack_bounds();
    miko_gc_set_clear();
    for (MikoGcBlock* each = miko_gc_head.next; each != &miko_gc_head; each = each->next) {
        miko_gc_set_insert((char*)each + sizeof(MikoGcBlock) + MIKO_HEADER_SIZE);
    }
    char* top = (char*)&registers;
    char* low = miko_gc_stack_low;
    char* high = miko_gc_stack_high;
    if (top < low) {
        low = top;
    }
    if (top > high) {
        high = top;
    }
    size_t words = ((size_t)(high - low)) / sizeof(void*);
    void** cursor = (void**)low;
    for (size_t i = 0; i < words; i++) {
        void* candidate = cursor[i];
        if (candidate == NULL) {
            continue;
        }
        /* The address set is the only filter: a word that is not the start of a
         * live block does nothing. No range test here - the heap sits above the
         * stack on most targets, so comparing against the stack bounds would
         * discard exactly the pointers this is looking for. */
        if (miko_gc_set_has(candidate)) {
            MikoGcBlock* block = (MikoGcBlock*)((char*)candidate - MIKO_HEADER_SIZE) - 1;
            block->marked = 1;
        }
    }

    /* Sweep: anything left unmarked is unreachable. */
    size_t live = 0;
    long long freed = 0;
    MikoGcBlock* block = miko_gc_head.next;
    while (block != &miko_gc_head) {
        MikoGcBlock* following = block->next;
        if (block->marked) {
            block->marked = 0;
            live = live + block->bytes;
        } else {
            block->next->prev = block->prev;
            block->prev->next = block->next;
            free(block);
            freed = freed + 1;
        }
        block = following;
    }

    miko_gc_live_bytes = live;
    miko_gc_freed_blocks = miko_gc_freed_blocks + freed;
    miko_gc_runs = miko_gc_runs + 1;
    /* Grow the next threshold with the live set so a program holding a lot of
     * data does not collect constantly, and keep a floor so a tiny heap does not
     * thrash. */
    size_t next = live * 2;
    if (next < miko_gc_threshold) {
        next = miko_gc_threshold;
    }
    miko_gc_next_collect = next;
}

/* Registers a freshly allocated block and returns the string pointer. */
static char* miko_gc_track(char* raw, size_t capacity) {
    MikoGcBlock* block = (MikoGcBlock*)raw;
    block->marked = 0;
    block->bytes = sizeof(MikoGcBlock) + MIKO_HEADER_SIZE + capacity + 1;
    block->next = miko_gc_head.next;
    block->prev = &miko_gc_head;
    miko_gc_head.next->prev = block;
    miko_gc_head.next = block;
    miko_gc_live_bytes = miko_gc_live_bytes + block->bytes;
    char* text = raw + sizeof(MikoGcBlock) + MIKO_HEADER_SIZE;
    return text;
}

/* Called after every allocation: collects when the heap has grown enough. */
static void miko_gc_step(void) {
    if (miko_gc_disabled) {
        return;
    }
    if (!miko_gc_ready) {
        miko_gc_init();
        if (miko_gc_disabled) {
            return;
        }
    }
    if (miko_gc_live_bytes >= miko_gc_next_collect) {
        miko_gc_collect();
    }
}

/* Byte length of a managed string. O(1). */
long long miko_str_len(const char* text) {
    if (text == NULL) {
        return 0;
    }
    return (long long)((const MikoStringHeader*)text)[-1].length;
}

/* Allocate an uninitialised managed string with room for `length` bytes plus a
 * terminator. Returns the character pointer, not the header.
 *
 * `capacity` is how many bytes may ever be written without moving the buffer;
 * it is >= length. Asking for spare capacity is what makes miko_str_append cheap
 * for repeated building. */
static char* miko_str_alloc_cap(size_t length, size_t capacity) {
    if (capacity < length) {
        capacity = length;
    }
    miko_gc_step();
    char* block = (char*)malloc(sizeof(MikoGcBlock) + MIKO_HEADER_SIZE + capacity + 1);
    if (!block) {
        fprintf(stderr, "out of memory allocating string of %zu bytes\n", length);
        exit(1);
    }
    char* text = miko_gc_track(block, capacity);
    MikoStringHeader* header = (MikoStringHeader*)(text - MIKO_HEADER_SIZE);
    header->length = length;
    header->capacity = capacity;
    text[length] = 0;
    return text;
}

/* Allocate with no spare room. */
static char* miko_str_alloc(size_t length) {
    return miko_str_alloc_cap(length, length);
}

/* Pointer to where the header lives, for code that must rewrite length. */
static MikoStringHeader* miko_str_header(const char* text) {
    return ((MikoStringHeader*)text) - 1;
}

/* Copy a foreign C string (argv, a library result) into a managed string so it
 * satisfies the header invariant. */
static char* miko_str_adopt(const char* source) {
    if (source == NULL) {
        return miko_str_alloc(0);
    }
    size_t length = strlen(source);
    char* text = miko_str_alloc(length);
    memcpy(text, source, length);
    return text;
}

/* Test-facing wrapper: the harness needs a managed string from a C literal, since
 * only strings built here (or emitted with a header by the code generator) carry
 * one. */
char* miko_str_adopt_public(const char* source) {
    return miko_str_adopt(source);
}

/* Grow or shrink a managed string. Used by read_file, which reads an unknown
 * number of bytes before it knows the length. */
static char* miko_str_resize(char* text, size_t length) {
    MikoStringHeader* header = miko_str_header(text);
    if (length <= header->capacity) {
        /* The buffer already has room, so nothing moves. */
        header->length = length;
        text[length] = 0;
        return text;
    }
    char* block = text - MIKO_HEADER_SIZE;
    char* raw = block - sizeof(MikoGcBlock);
    miko_gc_step();
    char* grown = (char*)realloc(raw, sizeof(MikoGcBlock) + MIKO_HEADER_SIZE + length + 1);
    if (!grown) {
        fprintf(stderr, "out of memory growing string to %zu bytes\n", length);
        exit(1);
    }
    /* realloc may have moved the block, so the address set has to follow it. The
     * old address is only reachable as a stale pointer now; dropping it is what
     * keeps the set from marking a block that no longer exists. */
    MikoGcBlock* gc = (MikoGcBlock*)grown;
    size_t previous = gc->bytes;
    gc->bytes = sizeof(MikoGcBlock) + MIKO_HEADER_SIZE + length + 1;
    miko_gc_live_bytes = miko_gc_live_bytes - previous + gc->bytes;
    char* newtext = grown + sizeof(MikoGcBlock) + MIKO_HEADER_SIZE;
    MikoStringHeader* grownHeader = (MikoStringHeader*)(newtext - MIKO_HEADER_SIZE);
    grownHeader->length = length;
    grownHeader->capacity = length;
    newtext[length] = 0;
    return newtext;
}

/* Append `src` to `dst` and return the string to use afterwards.
 *
 * Ownership: this takes over `dst`. The buffer may be reused in place, so any
 * *other* variable still pointing at the old value would observe the change.
 * The intended shape is `s = miko_str_append(s, piece);` - the old value is
 * discarded by the caller. A string literal has capacity == length, so it can
 * never be written into; appending to a literal always copies.
 *
 * Why this exists: `s = s + piece` in a loop is O(n^2), because every step
 * rebuilds the whole string. Building the compiler's own output that way cost
 * 70 seconds on a 94 KB input. Growth doubles the capacity, so a long run of
 * appends is amortised O(1) per byte. */
char* miko_str_append(char* dst, const char* src) {
    size_t dstLen = (size_t)miko_str_len(dst);
    size_t srcLen = (size_t)miko_str_len(src);
    if (srcLen == 0) {
        return dst;
    }
    size_t needed = dstLen + srcLen;
    MikoStringHeader* header = dst != NULL ? miko_str_header(dst) : NULL;
    if (header != NULL && needed <= header->capacity) {
        memcpy(dst + dstLen, src, srcLen);
        header->length = needed;
        dst[needed] = 0;
        return dst;
    }
    size_t capacity = header != NULL && header->capacity > needed ? header->capacity : needed;
    /* Double, so repeated appends do not reallocate every time. */
    if (capacity < needed * 2) {
        capacity = needed * 2;
    }
    char* grown = miko_str_alloc_cap(needed, capacity);
    if (dstLen > 0) {
        memcpy(grown, dst, dstLen);
    }
    memcpy(grown + dstLen, src, srcLen);
    grown[needed] = 0;
    return grown;
}

static char** miko_argv_storage = 0;

// Milliseconds from an arbitrary fixed origin, monotonic per process.
//
// Programs use this as a cheap entropy source (seeding a PRNG) and to measure
// elapsed time. Wall-clock time is deliberately avoided: it jumps whenever the
// system clock is adjusted, which corrupts both uses.
long long miko_mono_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int started = 0;
    LARGE_INTEGER now;
    if (!started) {
        QueryPerformanceFrequency(&freq);
        started = 1;
    }
    if (freq.QuadPart <= 0) {
        return 0;
    }
    QueryPerformanceCounter(&now);
    return (long long)((now.QuadPart / freq.QuadPart) * 1000
                       + ((now.QuadPart % freq.QuadPart) * 1000) / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + (long long)(ts.tv_nsec / 1000000);
#endif
}

void miko_set_args(int argc, char** argv) {
    miko_argc_storage = argc;
    miko_argv_storage = argv;
    /* The first call a generated main makes, so the collector learns the stack
     * bounds before any user code allocates. */
    miko_gc_init();
    miko_gc_install_report();
}

/* Explicit collection, and the counters the tests read. */
void miko_gc_collect_now(void) {
    if (miko_gc_disabled == 2) {
        return;
    }
    miko_gc_collect();
}

void miko_gc_debug_report(void) {
    fprintf(stderr, "[gc] ready=%d disabled=%d low=%p high=%p span=%lldKB live=%lld next=%lld runs=%lld\n",
            miko_gc_ready, miko_gc_disabled,
            (void*)miko_gc_stack_low, (void*)miko_gc_stack_high,
            (long long)((miko_gc_stack_high && miko_gc_stack_low)
                ? (miko_gc_stack_high - miko_gc_stack_low) / 1024 : 0),
            (long long)miko_gc_live_bytes, (long long)miko_gc_next_collect,
            miko_gc_runs);
}

long long miko_gc_run_count(void) {
    return miko_gc_runs;
}

long long miko_gc_freed_count(void) {
    return miko_gc_freed_blocks;
}

long long miko_gc_live_longs(void) {
    return (long long)miko_gc_live_bytes;
}

long long miko_arg_count(void) {
    if (miko_argc_storage <= 1) {
        return 0;
    }
    return (long long)(miko_argc_storage - 1);
}

char* miko_arg_at(long long index) {
    if (index < 0 || index >= miko_arg_count()) {
        fprintf(stderr, "arg_at index out of range\n");
        exit(1);
    }
    /* argv entries come from the OS with no header, so copy them into a managed
     * string; len(arg_at(0)) has to work like len on any other string. */
    return miko_str_adopt(miko_argv_storage[index + 1]);
}

void miko_print_i64(long long value) {
    printf("%lld\n", value);
}

void miko_print_bool(int value) {
    printf("%s\n", value ? "true" : "false");
}

void miko_print_str(const char* value) {
    printf("%s\n", value);
}

void miko_print_char(int value) {
    printf("%c\n", value);
}

// Diagnostics. The IR backend declares these so it can report a construct it
// cannot lower and then exit, instead of emitting IR that assembles but
// computes the wrong thing. stderr, one line each, matching print's shape.
void miko_eprint_str(const char* value) {
    fprintf(stderr, "%s\n", value);
}

void miko_eprint_i64(long long value) {
    fprintf(stderr, "%lld\n", value);
}

// Run a shell command and return its exit status. The self-hosted compiler uses
// this to invoke clang on the IR it just emitted, so compiling a program
// produces an executable instead of stopping at an intermediate .ll.
int miko_run_command(const char* command) {
    return system(command);
}

// 1 when the path can be opened for reading, 0 otherwise.
int miko_file_exists(const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    fclose(file);
    return 1;
}

char* miko_str_concat(const char* a, const char* b) {
    size_t na = (size_t)miko_str_len(a);
    size_t nb = (size_t)miko_str_len(b);
    char* out = miko_str_alloc(na + nb);
    memcpy(out, a, na);
    memcpy(out + na, b, nb);
    return out;
}

// A byte value in 0..255, not a C `char`.
//
// Plain `char` is signed on MSVC and on most other targets, so a UTF-8
// continuation byte (0x80..0xFF) would come back negative. Anything that does
// arithmetic on the result - building hex escapes, digit tables - then indexes
// out of bounds. Strings in this language are UTF-8 byte sequences, so a byte
// is what char_at means.
int miko_char_at(const char* text, long long index) {
    long long n = miko_str_len(text);
    if (index < 0 || index >= n) {
        fprintf(stderr, "char_at index out of range: index=%lld length=%lld text=[%.40s]\n",
                index, n, text);
        exit(1);
    }
    return (int)(unsigned char)text[index];
}

char* miko_substring(const char* text, long long start, long long length) {
    size_t n = (size_t)miko_str_len(text);
    if (start < 0 || length < 0 || (size_t)start > n) {
        fprintf(stderr, "substring range out of bounds\n");
        exit(1);
    }
    if ((size_t)length > n - (size_t)start) {
        length = (long long)(n - (size_t)start);
    }
    char* out = miko_str_alloc((size_t)length);
    memcpy(out, text + start, (size_t)length);
    return out;
}

char* miko_read_file(const char* path) {
    FILE* file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "failed to open file '%s'\n", path);
        exit(1);
    }
    if (fseek(file, 0, SEEK_END) != 0) { exit(1); }
    long size = ftell(file);
    if (size < 0) { exit(1); }
    rewind(file);
    char* out = miko_str_alloc((size_t)size);
    size_t read = fread(out, 1, (size_t)size, file);
    out[read] = 0;
    /* A short read leaves the recorded length too long, so fix it up. */
    if (read != (size_t)size) {
        out = miko_str_resize(out, read);
    }
    fclose(file);
    return out;
}

// Create every missing directory in the parent path of `path`.
//
// The interpreter already does this for write_file, so a program that writes
// out/p.ll works whether or not out/ exists. The compiled path has to behave the
// same way, otherwise `miko prog.miko out\p` fails on a fresh checkout purely
// because the output directory has not been made yet.
static void miko_make_parent_dirs(const char* path) {
    size_t n = strlen(path);
    if (n == 0 || n >= 4000) {
        return;
    }
    char buf[4000];
    memcpy(buf, path, n + 1);

    size_t last = 0;
    int found = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            last = i;
            found = 1;
        }
    }
    if (!found) {
        return;
    }
    // A separator at index 0 is a root, and at index 2 after "C:" is a drive
    // root; neither is something to create.
    for (size_t i = 1; i < last; i++) {
        if (buf[i] == '/' || buf[i] == '\\') {
            char saved = buf[i];
            buf[i] = '\0';
            miko_mkdir(buf);
            buf[i] = saved;
        }
    }
    char saved = buf[last];
    buf[last] = '\0';
    miko_mkdir(buf);
    buf[last] = saved;
}

void miko_write_file(const char* path, const char* content) {
    FILE* file = fopen(path, "wb");
    if (!file) {
        miko_make_parent_dirs(path);
        file = fopen(path, "wb");
    }
    if (!file) {
        fprintf(stderr, "failed to open file '%s' for writing\n", path);
        exit(1);
    }
    // A managed string may legitimately contain NUL bytes (read_file of a binary
    // file), so its length has to come from the header rather than from strlen.
    fwrite(content, 1, (size_t)miko_str_len(content), file);
    fclose(file);
}

char* miko_input(void) {
    /* fgets needs a raw buffer, but what comes back to the program has to be a
     * managed string: the old static buffer had no header, and every line read
     * overwrote the previous one. */
    static char buf[4096];
    if (!fgets(buf, sizeof buf, stdin)) {
        return miko_str_alloc(0);
    }
    size_t n = strlen(buf);
    if (n > 0 && buf[n-1] == 10) { n = n - 1; }
    if (n > 0 && buf[n-1] == 13) { n = n - 1; }
    buf[n] = 0;
    return miko_str_adopt(buf);
}

/* ---- value formatting, parsing and PRNG -----------------------------------
 *
 * These back the to_string_* / str_* / clock_ms / seed_i64 / rand_i64 builtins.
 * The three engines must agree, so the PRNG is SplitMix64 with exactly the same
 * constants as src/runtime/Interpreter.cpp and NativeCompiler::emitRand, and
 * failures report through the same "runtime error: <message>" wording the
 * generated IR uses for its own checks.
 */

char* miko_to_string_i64(long long value) {
    char raw[32];
    int written = snprintf(raw, sizeof raw, "%lld", value);
    if (written < 0) { written = 0; }
    char* text = miko_str_alloc((size_t)written);
    memcpy(text, raw, (size_t)written);
    return text;
}

char* miko_to_string_bool(int value) {
    /* Copied into a managed string instead of returning a static literal:
     * len(to_string_bool(true)) reads the header. */
    return miko_str_adopt(value ? "true" : "false");
}

char* miko_to_string_char(int value) {
    char* text = miko_str_alloc(1);
    text[0] = (char)value;
    return text;
}

/* Whole-string integer parse: the entire text has to be consumed, so "12x" and
 * "" are both errors rather than partial reads. */
long long miko_str_to_i64(const char* text) {
    char* end = 0;
    long long value = strtoll(text, &end, 10);
    if (end == text || (end && *end != 0)) {
        fprintf(stderr, "runtime error: %s\n", "invalid integer");
        exit(1);
    }
    return value;
}

// Order two strings the way `<`, `==` and `!=` mean them.
//
// Compares by header length, not by strcmp: a string may now legitimately hold
// NUL bytes (read_file of a binary), and strcmp would silently treat the first
// NUL as the end. The ordering itself is byte-lexicographic, so a shorter string
// that is a prefix of a longer one sorts first - the same answer strcmp gives.
int miko_str_cmp(const char* a, const char* b) {
    long long na = miko_str_len(a);
    long long nb = miko_str_len(b);
    long long shared = na < nb ? na : nb;
    int order = shared > 0 ? memcmp(a, b, (size_t)shared) : 0;
    if (order != 0) {
        return order < 0 ? -1 : 1;
    }
    if (na == nb) {
        return 0;
    }
    return na < nb ? -1 : 1;
}

// First byte offset at which `needle` occurs in `text`, or -1.
//
// Length-aware rather than strstr for the same reason as miko_str_cmp: strstr
// stops at the first NUL in both arguments.
long long miko_str_index_of(const char* text, const char* needle) {
    long long n = miko_str_len(text);
    long long m = miko_str_len(needle);
    if (m == 0) {
        return 0;
    }
    if (m > n) {
        return -1;
    }
    long long last = n - m;
    long long at = 0;
    while (at <= last) {
        if (text[at] == needle[0] && memcmp(text + at, needle, (size_t)m) == 0) {
            return at;
        }
        at = at + 1;
    }
    return -1;
}

int miko_str_contains(const char* text, const char* needle) {
    return miko_str_index_of(text, needle) >= 0;
}

/* Wall-clock milliseconds since the Unix epoch. miko_mono_ms above is the
 * monotonic counterpart; this one is for reporting a calendar time. */
long long miko_clock_ms(void) {
#ifdef _WIN32
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;
    /* 100ns ticks since 1601-01-01; shift to the Unix epoch, then to ms. */
    unsigned long long since_epoch = ticks.QuadPart - 116444736000000000ULL;
    return (long long)(since_epoch / 10000ULL);
#else
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return (long long)now.tv_sec * 1000LL + (long long)(now.tv_nsec / 1000000L);
#endif
}

static unsigned long long miko_rng_state = 0x9E3779B97F4A7C15ULL;

void miko_seed_i64(long long seed) {
    miko_rng_state = (unsigned long long)seed;
}

long long miko_rand_i64(void) {
    unsigned long long z = (miko_rng_state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB13511EF9ULL;
    z = z ^ (z >> 31);
    return (long long)z;
}

/* ---- floating point ------------------------------------------------------
 *
 * print uses "%g" and to_string_f64 uses "%.12g", matching the interpreter and
 * NativeCompiler so the three engines format a double identically.
 */

void miko_print_f64(double value) {
    printf("%g\n", value);
}

char* miko_to_string_f64(double value) {
    char raw[64];
    int written = snprintf(raw, sizeof raw, "%.12g", value);
    if (written < 0) { written = 0; }
    char* text = miko_str_alloc((size_t)written);
    memcpy(text, raw, (size_t)written);
    return text;
}

double miko_str_to_f64(const char* text) {
    char* end = 0;
    double value = strtod(text, &end);
    if (end == text || (end && *end != 0)) {
        fprintf(stderr, "runtime error: %s\n", "invalid float");
        exit(1);
    }
    return value;
}

/* Fatal diagnostic used by the generated IR for its own guards (division by
 * zero, index out of range, and so on). Living here rather than in the emitted
 * IR keeps the IR free of platform symbols: MSVC spells stderr as
 * __acrt_iob_func(2), which does not exist elsewhere, so a program built on one
 * platform could not even be linked on another. */
void miko_runtime_error(const char* message) {
    fprintf(stderr, "runtime error: %s\n", message);
    exit(1);
}
