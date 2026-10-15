<?php
/*
 * Replays tests/vectors.json through the real FRAS library and reports any
 * drift. This is the PHP half of the shared-vector check; tests/vectors_test.c
 * is the C half.
 *
 * Two things can fail here, and they mean opposite things:
 *
 *   - An "agreed" vector no longer matching means the two implementations
 *     have drifted apart and weights fitted by this trainer are no longer
 *     valid for what production computes. That is a bug.
 *
 *   - A "divergent" vector now matching means somebody fixed the PHP side
 *     (see tests/README.md). That is good news, and the fix is to
 *     regenerate the vectors so the entry moves into "agreed".
 *
 * Run: php -d extension=mbstring tools/check_php.php tests/vectors.json
 */

$frasPath = __DIR__ . '/../../FRAS Algorithm/src/FRAS.php';
if (!is_readable($frasPath)) {
    fwrite(STDERR, "skip: the FRAS library is not at {$frasPath}\n");
    fwrite(STDERR, "      this check needs the sibling project checked out beside this one\n");
    exit(0);
}
if (!function_exists('mb_strtolower')) {
    fwrite(STDERR, "skip: ext-mbstring is not loaded; FRAS cannot run without it\n");
    exit(0);
}
require_once $frasPath;

$path = $argv[1] ?? __DIR__ . '/../tests/vectors.json';
$doc = json_decode(file_get_contents($path), true);
if (!is_array($doc)) {
    fwrite(STDERR, "error: could not read vectors from {$path}\n");
    exit(1);
}

function phpComponents(string $q, string $f): array {
    $out = [];
    foreach ([[1, 0, 0], [0, 1, 0], [0, 0, 1]] as $w) {
        $fras = new FRAS($q, [['v' => $f]], (float) $w[0], (float) $w[1], (float) $w[2]);
        $r = $fras->look('v');
        $out[] = $r[0]['similarityScore'];
    }
    return ['prefix' => $out[0], 'lcs' => $out[1], 'levenshtein' => $out[2]];
}

function differs(array $got, array $want): bool {
    foreach (['prefix', 'lcs', 'levenshtein'] as $k) {
        if (abs($got[$k] - (float) $want[$k]) > 5e-7) return true;
    }
    return false;
}

function describe(array $c): string {
    return sprintf('prefix=%.6f lcs=%.6f lev=%.6f', $c['prefix'], $c['lcs'], $c['levenshtein']);
}

$checked = 0;
$failed = 0;

foreach ($doc['agreed'] ?? [] as $i => $v) {
    $checked++;
    $got = phpComponents($v['query'], $v['field']);
    if (differs($got, $v['expected'])) {
        $failed++;
        printf("DRIFT  agreed vector %d  q=\"%s\" f=\"%s\"\n", $i, $v['query'], $v['field']);
        printf("       PHP now gives %s\n", describe($got));
        printf("       vectors.json expects %s\n", describe(array_map('floatval', $v['expected'])));
    }
}

$nowAgreeing = [];
foreach ($doc['divergent'] ?? [] as $i => $v) {
    $checked++;
    $got = phpComponents($v['query'], $v['field']);
    if (differs($got, $v['php'])) {
        $failed++;
        if (!differs($got, $v['c'])) {
            $nowAgreeing[] = $i;
            printf("FIXED  divergent vector %d  q=\"%s\" f=\"%s\"\n", $i, $v['query'], $v['field']);
            printf("       PHP now agrees with this trainer: %s\n", describe($got));
        } else {
            printf("DRIFT  divergent vector %d  q=\"%s\" f=\"%s\"\n", $i, $v['query'], $v['field']);
            printf("       PHP now gives %s\n", describe($got));
            printf("       vectors.json recorded %s\n", describe(array_map('floatval', $v['php'])));
        }
    }
}

printf("\n%d vectors checked against the real FRAS library, %d differ\n", $checked, $failed);

if ($nowAgreeing) {
    printf("\n%d previously divergent vector(s) now agree. If FRAS::look() was fixed to\n"
        . "lower-case consistently before the LCS and Levenshtein passes, regenerate the\n"
        . "vectors so those entries move into \"agreed\":\n\n"
        . "    make vectors\n\n", count($nowAgreeing));
}

exit($failed === 0 ? 0 : 1);
