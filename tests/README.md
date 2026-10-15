# Tests

```sh
make test          # build and run the suite          (no PHP needed)
make check         # the above, plus the PHP-side vector replay
```

`make test` expects 394 checks and 0 failures. Everything it needs is
committed: a C11 compiler and pthreads are the only requirements.

## What is here

| File | Covers |
| --- | --- |
| `scoring_test.c` | UTF-8 decoding, case folding, token splitting, the two DP passes |
| `json_test.c` | the parser, including every allocation failing in turn |
| `fit_test.c` | the grid search, its tie-breaks, and thread-count independence |
| `vectors_test.c` | replays `vectors.json` |
| `probe.c` | prints this project's scores for pairs on stdin; used by the generators |
| `harness.h` | assertion macros and counters |
| `alloc_hooks.h` | declarations for the failing allocator, force-included into `json.c` |

## The shared vectors

This trainer only has value if it computes the same three numbers as
`FRAS::look()` does in production. If the two drift apart, the trainer fits
weights for a function nobody runs, and nothing anywhere reports an error —
the rankings just get quietly worse.

`vectors.json` is what makes that impossible to miss. Its expected values
were produced by running the real PHP class, not by describing what it
ought to do: `FRAS::look()` does not expose the three components, so
`tools/gen_vectors.php` runs each pair through it three times with the
weights `[1,0,0]`, `[0,1,0]` and `[0,0,1]`, at which point
`similarityScore` *is* the component. `tests/vectors_test.c` asserts the C
side against that file and `tools/check_php.php` replays it through PHP, so
whichever side moves first, something fails.

The file has two groups.

**`agreed`** — pairs where the two implementations produce identical
numbers. This is the contract, and both sides assert it.

**`divergent`** — pairs where they do not. It is empty, and
`vectors_test.c` fails if it is not: an entry there is a disagreement nobody
has explained, and weights fitted while it stands are fitted to the wrong
function.

It held eight entries until 2026-09-30, all with one cause, a bug in the PHP
library: `FRAS::look()` lower-cased the query and the field for the prefix
comparison but handed the **original-case** strings to `LCS` and
`Levenshtein`, so two of its three components were case-sensitive. This
trainer had folded case consistently all along. `FRAS Algorithm` was fixed
to match, and the eight moved into `agreed` with this trainer's numbers
unchanged.

The same pass found a defect both sides shared. The token-boundary bonus
skipped token 0 because the literal prefix covers it, which is true only
when the field *starts* with that word. After a leading delimiter
(`" smith"`, `".net framework"`) neither check saw the first word. Both were
fixed together, and four leading-delimiter pairs were added to the vectors.

`check_php.php` prints `FIXED` rather than `DRIFT` when a divergent vector
starts agreeing. If that happens, run `make vectors` to move those entries
into `agreed`.

## How well the two agree now

`tools/differential.php` generates random pairs from Latin, Latin-1, Greek
and Cyrillic letters in both cases, the dotted and dotless I, and every kind
of token delimiter, and compares all three components:

```sh
sh tests/build.sh
php -d extension=mbstring tools/differential.php tests/build/probe [count] [seed]
```

With the default 4,000 pairs and seed:

| | agree | differ |
| --- | --- | --- |
| pairs already lower-case | 2000 | 0 |
| mixed-case pairs | 2000 | 0 |

20,000 pairs on seed 7 agree as well. Pointed at the PHP as it was before
the fix (the fourth argument is a path to a `FRAS.php`), the same run
reports 732 mixed-case pairs differing, and 15 pairs differing on prefix,
7 of them already lower-case. Those 15 are the token-0 defect, which a
differential could not find while both sides had it.

## The failing allocator

`json_test.c` is the reason `src/json.c` routes its allocations through
`JSON_MALLOC`/`JSON_CALLOC`/`JSON_REALLOC` macros. They are the real
functions in every normal build; the test build points them at a counting
allocator that can be told to fail the *n*th allocation. The suite then
parses several documents once per allocation site and requires a clean
failure each time.

That is not decoration. Against the original parser, which checked none of
its seven allocation sites, **every one of those injected failures was an
access violation.** It also caught a NULL dereference introduced during
this pass, in the `true`/`false` branch of `parse_value` where the result
of `alloc_value` was written through before being checked.

## Regenerating

Both generated files are committed, so neither a build nor a test run needs
PHP. Regenerate them only when the PHP side changes:

```sh
make tables     # src/unicode_tables.h, from mbstring itself
make vectors    # tests/vectors.json, from the real FRAS::look()
```

A non-empty diff from `make tables` after a PHP upgrade means production's
case folding moved and previously fitted weights are stale.

Both generators take an output *path* rather than writing to stdout. That
is deliberate: PowerShell's `>` writes a UTF-8 BOM, and a BOM at the top of
a generated file is invisible in every diff that would have caught it. One
cost an hour during this pass.
