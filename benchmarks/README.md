# benchmarks

Programs that measure one thing each. They exist because "fast" is a claim, not a
property: without a number in front of it, a language can regress by 30x and
nobody notices.

Every `.miko` here has a same-shaped `.go` beside it except `str_scan.miko` and
`str_build.miko`, which measure language-level defects rather than comparing
against Go.

Run one:

```
dist\miko.exe benchmarks\str_scan.miko out\str_scan      # -O2 via dist\miko-link.bat
out\str_scan.exe
```

## str_scan

`str_scan.txt` is 20 000 bytes of `abcdefghij` repeated. `str_scan.miko` reads it
with `read_file`, then scans it 20 times with

```
while i < n { total = total + char_at(s, i); i = i + 1; }
```

The string has to come from a file rather than a literal: with a literal, clang
sees the constant length and hoists every `strlen` out of the loop, which hides
exactly the defect this benchmark exists to show.

What it caught: `len()` was `strlen()`, and `char_at` computed `strlen` itself to
bounds-check. So each character visit re-walked the whole string, and the loop was
O(n²) while touching each byte once.

| | before | after |
|---|---|---|
| 20 KB, 20 scans | 278 ms | 8 ms |
| `strlen` in emitted IR | present | none |
| n = 10k / 20k / 40k | quadratic | 8 / 8 / 7 ms |

The fix put the byte length in a header before the text (see
`MikoStringHeader` in `runtime/miko_runtime.c`), so the length is a load. The
flat row is the point: 4x the input, same time, because the per-character cost no
longer depends on the string's size.

## str_build

`str_build.miko` builds the same string two ways: `s = s + piece` and
`s = append_str(s, piece)`. This is the write-side counterpart to `str_scan`, and
it was the second half of the same defect - `str_scan` fixed reading, this fixed
writing.

| n (pieces) | `s + piece` | `append_str(s, piece)` |
|---|---|---|
| 20 000 | 85 ms | <1 ms |
| 40 000 | not measured | <1 ms |
| 100 000 | not measured | <1 ms |
| 400 000 | not measured | 1 ms |
| 1 600 000 | not measured | 4 ms |

Reproduce with:

```
out\str_build.exe 20000           # both paths
out\str_build.exe 1600000 append
```

The concat column stops at 20 000 on purpose: the shape is quadratic, so 8x that
input is roughly 64x the time, about 5 seconds, and the extra points would only
restate what one point already proves. The program itself refuses to run the
concat path above 20 000 for that reason.

The append column is flat where it matters: 16x the work (100 000 to 1 600 000)
costs 4 ms more, and the per-piece cost keeps falling. The buffer grows by
doubling and the append writes in place, so a long run of appends is amortised
O(1) per byte.

The `<1 ms` entries are below the 1 ms resolution of `mono_ms`, which is why the
larger sizes are in the table: a benchmark reporting "0 ms" at every size it
tries has stopped measuring.

### The cost this removed from the compiler

The self-hosted compiler accumulated its token stream, AST dump and IR text with
`stream = stream + piece`. Every one of those is a quadratic loop over a growing
string, so the compiler was unusable past about 50 KB of input.

| input | before | after |
|---|---|---|
| 23 337 B (200 functions) | 2 173 ms | 291 ms |
| 46 737 B (400 functions) | 6 408 ms | 298 ms |
| 93 537 B (800 functions) | 70 375 ms | 320 ms |
| `compiler/main_ll.miko` (29 096 B) | - | 279 ms |
| `compiler/codegen_ll.miko` (142 275 B) | 67 245 ms (did not finish linking) | 1 780 ms |

The before column is not a smooth curve: 2x the input cost 3x, then 11x. That is
the signature of a quadratic loop, and it is why the compiler felt like it hit a
wall rather than slowed down gradually.

The after column is essentially flat across 4x of input because what remains in
those 300 ms is process start plus the clang link, not compilation. Measured per
phase on the 800-function input, before and after:

| phase | before | after |
|---|---|---|
| lex | 47 074 ms | 17 ms |
| parse | 5 763 ms | 17 ms |
| sema | 30 ms | 18 ms |
| static rules | 16 ms | 3 ms |
| codegen | 17 ms | 8 ms |

Lexing was 85% of the time. `token.append_token` did
`return stream + kind + "|" + ... + "\n"`, rebuilding a multi-megabyte stream for
every one of ~40 000 tokens.

## fib / loop / nest / perf_compare

Straight comparisons against Go, kept for regression watching rather than
headline numbers. `perf_compare.miko` and `perf_compare.go` are the pair used when
quoting a ratio.

## Known gap

Nothing here measures allocation volume or peak memory. `append_str` removes the
repeated copying but the buffers are never freed, so a long-running program still
grows. Measuring that needs a way to report peak RSS, and fixing it needs the
reclamation half of item 7 in `ROADMAP.md` - `append_str` is the ownership rule,
not the reclamation strategy.
