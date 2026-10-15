# Code Review — fras-weight-trainer

First reviewed 2026-09-04. **Remediated 2026-09-18** on branch
`cleanup/2026-09-18`; this file is now a record rather than a to-do list.
The original review's text is preserved under "Original findings" below,
with what actually happened to each item.

## Follow-up, 2026-09-30: the divergence is closed, and there was a second one

One commit on this branch, made during the `FRAS Algorithm` pass because the
two projects have to move together. Everything below this section is the
2026-09-18 record and is left as written; where it says the PHP bug is open,
read this instead.

**The PHP case bug is fixed** in `FRAS Algorithm`, exactly as predicted at
the end of this file. `check_php.php` printed `FIXED` for all eight divergent
vectors, including the length-changing dotted I, and they are now in
`agreed` with this trainer's numbers unchanged — this side had been right.

**The 4,000-pair differential had missed a defect both sides shared.** The
token-boundary bonus skipped token 0, in PHP and here alike, on the grounds
that the literal prefix covers it. That holds only when the field starts
with that word. After a leading delimiter — `" smith"`, `".net framework"`,
`"---acme"` — neither check saw the first word and the prefix scored 0. A
differential compares two implementations with each other, so a bug they
share is invisible to it; this one was found by reading `FRAS::look()`.
`token_bonus_score()` now includes token 0 at full weight, like the PHP.

What changed here:

- `src/scoring.c`: the token-0 skip removed. Four new checks in
  `scoring_test.c`, confirmed failing first; **394 checks** now, up from 386.
- `tests/vectors.json`: regenerated. **46 agreed, 0 divergent.** Compared
  entry by entry with the previous file: all 34 old agreed values unchanged,
  all 8 old divergent entries now carry their old `c` values, and the only
  additions are four leading-delimiter pairs.
- `tests/vectors_test.c`: the check that the known divergence was still
  recorded is inverted. The file must now record **no** divergence, because
  any entry there is unexplained.
- `tools/differential.php`: the random differential, committed so the
  numbers can be reproduced. 4,000 pairs and 20,000 pairs on a second seed:
  0 differ. Against the PHP before the fix: 732 mixed-case pairs, plus 15 on
  prefix that are the token-0 defect.

**Generating vectors on Windows:** pass the probe as `tests\build\probe.exe`.
`gen_vectors.php` opens it through `proc_open` with a command string, which
goes via `cmd.exe`, and `tests/build/probe` fails with "fewer rows than there
are pairs" because cmd reads the slashes as switches. `differential.php` uses
the array form and resolves the path itself, so it has no such trap.

## Correction to the first review

The first review was right about both P1s and about every P3. One P3 needs
a correction of degree, not of substance:

**Finding 4 said a deeply nested file "recurses until the stack runs out".
True, but not at any depth a first attempt reaches.** The original parser
handled 5,000 levels of nesting without complaint and 20,000 levels too. It
dies at 50,000 with `STATUS_STACK_OVERFLOW` (0xC00000FD). A regression test
written against the review's wording — 5,000 brackets, expecting a crash —
passes against the *unfixed* code and proves nothing. The committed test
asserts rejection rather than a crash, which is a claim about the cap that
now exists; the 50,000-level measurement is recorded here because it is the
part that had to be found by experiment.

## The headline: the two implementations did not agree, and now differ in exactly one place

Finding 2 predicted that the C and PHP scoring would drift and that nobody
would notice. It had already drifted, in three separate ways, and two of
them were this project's fault. They were found by generating 4,000 random
`(query, field)` pairs and running them through both implementations.

**1. Case folding stopped at Latin-1 (this project's bug, fixed).** The
decoder folded ASCII and the Latin-1 supplement and nothing else, so a
Greek or Cyrillic query scored a literal prefix of **0.0 against its own
field** where PHP scored **1.0**. The README documented this as a minor
limitation for "scripts with more exotic casing rules". It is not minor:
for any non-Western dataset the prefix component was noise, and the fitted
prefix weight therefore meaningless.

**2. Token splitting was ASCII-only (this project's bug, fixed).** FRAS
splits field tokens on `preg_split('/[\s\-_,.\/]+/u', ...)`, and the `/u`
modifier makes `\s` Unicode-aware. A field separated by a no-break space,
an ideographic space, a thin space or NEL is two tokens in PHP and one
here, and the token bonus is only awarded from token 1 onwards — so those
fields scored 0.0 where PHP scored 0.5.

**3. `FRAS::look()` is case-sensitive in two of its three components (the
PHP library's bug, NOT fixed here).** This is the one that matters:

> `FRAS::look()` computes `$queryLower`/`$fieldLower` and uses them for the
> prefix comparison, then passes `$this->query` and `$fieldValue` —
> original case — to `new LCS(...)` and `new Levenshtein(...)`. Neither
> class lower-cases internally. So prefix is case-insensitive and LCS and
> Levenshtein are case-sensitive.

It is a slip rather than a design: the character-overlap short-circuit that
decides *whether to run the LCS at all* uses the lower-cased sets, so the
surrounding code assumes the folding it does not do. The effect is not
subtle. `"ACME"` against `"acme-corp"` gives `lcs=0.000, lev=0.000` in PHP
and `1.000, 0.444` here; `"john"` against `"John Smith"` gives `0.750` and
`1.000`.

**This was left alone deliberately.** `FRAS Algorithm` is a separate
project with its own review, and the standing instruction in `CLAUDE.md` is
one project at a time. Making this trainer case-sensitive to match would
have meant propagating a bug into a second codebase and reverting it later.
Instead the divergence is pinned down so it cannot be missed — see "What is
left" at the end.

### Where the two stand now

Over 4,000 random pairs (Latin, Latin-1, Greek, Cyrillic, Unicode spaces,
dotted I):

| | agree | differ |
| --- | --- | --- |
| pairs already lower-case | 2000 | 0 |
| mixed-case pairs | 1278 | 722 |

Per component across all 4,000: **prefix 0 mismatches**, lcs 564,
levenshtein 603. Everything that remains is the PHP case bug and nothing
else.

## What was fixed

**Every allocation in `json.c` is checked, and the three `realloc`
self-assignments are gone (P1).** Confirmed the hard way: `src/json.c` now
routes allocations through macros that the test build points at a counting
allocator, and the suite parses several documents once per allocation site.
Against the original parser **every injected failure was an access
violation** — all seven sites, not just the three the review named. That
harness also caught a NULL dereference introduced *during* this pass, in
the `true`/`false` branch of `parse_value`, where `alloc_value`'s result
was written through before being checked.

**Shared scoring test vectors, asserted from both ends (P1).**
`tests/vectors.json` holds 42 `(query, field)` expectations whose values
were produced by running the real `FRAS::look()` — three times per pair
with the weights `[1,0,0]`, `[0,1,0]`, `[0,0,1]`, at which point
`similarityScore` is exactly the component wanted. `tests/vectors_test.c`
asserts the C side; `tools/check_php.php` replays the same file through
PHP. The file separates `agreed` vectors from the 8 known-`divergent`
ones, recording both answers for each of the latter, so the checker reports
`FIXED` rather than `DRIFT` when somebody fixes the PHP.

**The Unicode tables are generated from `mbstring` itself.**
`tools/gen_unicode_tables.php` probes PHP for the full simple lowercase
mapping (1,459 entries), the `Cased` and `Case_Ignorable` sets needed for
the Greek final-sigma rule, and the exact set PCRE treats as `\s` under
`/u`. Agreement is then true by construction rather than by careful
copying, and re-running it after a PHP upgrade is how you find out that
production's scoring moved. Three things it made reachable that hand-written
tables would likely have missed:

- Greek final sigma is context-sensitive: `ΟΔΟΣ` → `οδος` with U+03C2, but
  `ΣΟΦΙΑ` → `σοφια` with U+03C3. A per-character table alone gets it wrong.
- U+0130 lower-cases to **two** codepoints (`i` + combining dot above), so
  the folded string is *longer* than the input. String length is the
  denominator of both the LCS and Levenshtein scores.
- Undecodable bytes become mbstring's substitute character `?`, not U+FFFD,
  and one substitute covers a whole maximal invalid subpart rather than one
  per byte — `"a\xE2\x82b"` is three characters, not four.

**A test target covering the decoder and the grid search (P2).** 386 checks
in `tests/`, no framework, no PHP needed to run them.

**A nesting depth limit (P3).** Capped at 200; the format is two levels
deep.

**The broken README link (P3).** It pointed at `../Algorithm/README.md`;
the directory is `FRAS Algorithm`.

**A warning on training sets too small to trust (P3).** Below 30 sessions
the tool now says plainly that the reported MRR is measured on the data it
was fitted to and is an upper bound. A held-out split was considered and
not done: it would change which weights get written, which is a bigger
decision than a warning.

**`LICENSE` (P3).** MIT, matching the other remediated projects.

## Fixed but not in the original review

**The tool did not build on Windows at all.** `main.c` called
`sysconf(_SC_NPROCESSORS_ONLN)`, which MinGW does not have, so the default
thread count was a hard compile error. Everything else was already portable
— MinGW supplies pthreads through winpthreads. Now behind a platform split.

**A failed `pthread_create` produced a silently wrong fit.** The return
value was unchecked, so a slice of the weight grid went unsearched while
its zeroed result still took part in choosing the winner, and
`pthread_join` was called on an uninitialised handle. Failed slices now run
on the calling thread.

**A failed write produced a success report.** The output file is the entire
product of this tool, and `fclose` was called without checking it. A full
disk gave a truncated weights file, "Wrote ..." on stdout, and exit 0.

**`01` parsed as the number `1`.** JSON forbids leading zeros and PHP's
`json_decode` rejects them, so a typo in a training file silently changed a
value instead of being reported.

**`json.h` documented the opposite of what `json_parse` does.** It said the
error string "caller does not need to free it"; it is `strdup`'d and
`main.c` frees it. A future caller following the header would have leaked.

**A UTF-8 BOM made a training file unparseable** with "unexpected character
(line 1, column 1)", pointing at a byte that is invisible in every editor.
Now skipped.

**Several allocation failures were conflated with malformed input.**
`load_candidate` returning "skip this candidate" on an allocation failure
would have fitted weights against a quietly truncated training set; a
failed `strdup` of a profile name would have silently moved a session out
of its profile group and into `global` only. Both are now distinct, fatal,
and reported.

## Structural change

`fit.c`/`fit.h` were split out of `main.c` — the grid search, the
evaluation function and the tie-break comparison, none of which knew
anything about JSON or files but all of which were unreachable from a test.
`main.c` is now argument parsing, loading and output. This is the
`Web/PHP/eRAS/utils/` pattern the archive already uses.

The one behavioural property worth calling out is that the fitted triple is
independent of `--threads`, which is what the lexicographic tie-break
exists for. There is now a test asserting it across 1, 2, 3, 4, 7, 16 and
64 threads, and it is the sort of thing that would otherwise only surface
on a machine with a different core count.

## Deliberately not done

**The PHP case-sensitivity bug.** Belongs to `FRAS Algorithm`. See above.

**A held-out validation split.** The review offered this or a warning; the
warning was taken. A split changes which weights get written, and with the
training sets this tool is likely to see (the bundled example has 12
sessions) it would mostly mean fitting on 8 sessions instead of 12. That is
a judgement about the tool's purpose, not a bug fix.

**The `fit.c` tests are characterisation tests, not regressions.** The
search logic was moved without changing it, so those 81 checks pass against
the original logic too. They are worth having — they pin the tie-break and
the thread-independence property — but they did not find anything, and this
file should not imply otherwise. The two real fixes in that area (the
`pthread_create` fallback and refusing an empty session set) are not
reachable from a test without a thread-creation failure.

**`write_weight_object` indents its closing brace oddly** in the nested
profile case. It is output formatting, not business logic.

## Original findings, and their outcome

| # | Finding | Outcome |
| --- | --- | --- |
| 1 | P1 — unchecked allocations, three `realloc` self-assignments | Fixed; all seven sites, with a failing-allocator test |
| 2 | P1 — scoring duplicated in two languages with nothing checking agreement | Fixed as far as this side can: two C-side divergences closed, shared vectors added, one PHP-side bug documented |
| 3 | P2 — no tests at all | Fixed; 386 checks |
| 4 | P3 — no JSON nesting depth limit | Fixed; see the correction above on the depth at which it actually failed |
| 5 | P3 — broken README link | Fixed |
| 6 | P3 — no overfitting discussion | Warning added; held-out split deliberately not done |
| — | P3 — no `LICENSE` | Added (MIT) |

## Verifying

```sh
make test     # 394 checks, 0 failed -- no PHP needed
make check    # the above, plus replaying the vectors through the real PHP
```

`make check` needs `php` with `ext-mbstring` and the sibling
`FRAS Algorithm` checkout beside this directory; it skips with a message
rather than failing if either is missing. On this machine mbstring is not
enabled in `php.ini`, so it needs:

```sh
php -d extension_dir=D:\php-8.4.14-Win32-vs17-x64\ext -d extension=mbstring tools/check_php.php tests/vectors.json
```

Expect `46 vectors checked against the real FRAS library, 0 differ`.

The random differential, which needs the probe built first:

```sh
php -d extension_dir=D:\php-8.4.14-Win32-vs17-x64\ext -d extension=mbstring tools/differential.php tests/build/probe
```

Expect 2000 and 2000 agreeing, 0 differing.

## What is left, for whoever is next

Nothing with a live consequence. The PHP case bug that this section used to
describe was fixed on 2026-09-30 — see the follow-up at the top.

**Refit any weights file produced before 2026-09-30.** Its case handling
already matches today's PHP, because this side always folded case. Its
token-0 handling does not: fields with a leading delimiter now score higher
on prefix than they did when it was fitted. The difference is confined to
those fields, but refitting is one command. No such file is in either
repository.

`FRAS Algorithm` now carries a copy of `tests/vectors.json` and asserts it
from PHPUnit, and checks that its copy matches this one when both are
checked out side by side. Regenerate here, then copy it there.

## Licence, 2026-10-02

The author chose one licence for all of their projects: free for personal,
non-commercial use, modifiable for the user's own use, and not to be
distributed, original or modified. No standard licence says that, so `LICENSE`
is a short custom text, the same in every project apart from the year and what
it excludes. The README's licence section says the same. Anything above that
calls the licence an open question, or calls the project MIT, is superseded.

## CI, 2026-10-02

`.github/workflows/ci.yml` runs `make test` on `ubuntu-latest` and on
`windows-latest` in MSYS2's MINGW64 shell. Nothing has been pushed, so it has
not run on GitHub yet. Every job was run before committing, from a fresh clone
of this branch, the way a runner would run it: Linux jobs in a throwaway
Ubuntu 24.04 (the image `ubuntu-latest` uses), Windows jobs here, each with
only the tools the job sets up. 394 on each.

Not Git's bash on Windows: its `bin\bash.exe` puts Git's own `mingw64\bin`
first on `PATH`, and a MinGW `cc1` started from it here loaded Git's
`zlib1.dll` and `libzstd.dll` and exited 1 with no message at all.
