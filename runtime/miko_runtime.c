/* miko_runtime.c - C runtime services for the miko -> LLVM IR backend.
 *
 * The IR emitted by compiler/codegen_ll.miko declares these functions and
 * calls them; keeping print / string / file / argv logic in C keeps the IR
 * generator small. Compiled together with the generated .ll (or .obj) by
 * clang, or ahead of time and linked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <direct.h>
static void miko_mkdir(const char* path) {
    _mkdir(path);
}
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
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
    char* block = (char*)malloc(MIKO_HEADER_SIZE + capacity + 1);
    if (!block) {
        fprintf(stderr, "out of memory allocating string of %zu bytes\n", length);
        exit(1);
    }
    MikoStringHeader* header = (MikoStringHeader*)block;
    header->length = length;
    header->capacity = capacity;
    char* text = block + MIKO_HEADER_SIZE;
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
    char* grown = (char*)realloc(block, MIKO_HEADER_SIZE + length + 1);
    if (!grown) {
        fprintf(stderr, "out of memory growing string to %zu bytes\n", length);
        exit(1);
    }
    MikoStringHeader* grownHeader = (MikoStringHeader*)grown;
    grownHeader->length = length;
    grownHeader->capacity = length;
    char* out = grown + MIKO_HEADER_SIZE;
    out[length] = 0;
    return out;
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
