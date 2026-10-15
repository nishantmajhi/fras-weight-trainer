<?php
/*
 * Random differential between this trainer's scoring and FRAS::look().
 *
 * tests/vectors.json pins a few dozen chosen pairs. This generates thousands
 * of random ones from an alphabet built to hit what can move a score -
 * case in both directions, Latin-1, Greek (including final sigma), Cyrillic,
 * the length-changing dotted I, and every kind of token delimiter, leading
 * ones included - and reports every pair on which the two disagree.
 *
 * Run: php -d extension=mbstring tools/differential.php <probe> [count] [seed] [path/to/FRAS.php]
 *
 * <probe> is tests/build/probe, built by tests/build.sh. Exits 1 if any pair
 * differs.
 */

if ($argc < 2) {
    fwrite(STDERR, "usage: php tools/differential.php <probe> [count] [seed] [path/to/FRAS.php]\n");
    exit(2);
}
// CreateProcess resolves neither a relative forward-slash path nor a
// missing ".exe", so hand it an absolute path to a file that exists.
$probe = realpath($argv[1]) ?: realpath($argv[1] . '.exe');
if ($probe === false) {
    fwrite(STDERR, "error: no probe at {$argv[1]}; build it with tests/build.sh\n");
    exit(2);
}
$count = (int) ($argv[2] ?? 4000);
$seed = (int) ($argv[3] ?? 20260930);
$frasPath = $argv[4] ?? __DIR__ . '/../../FRAS Algorithm/src/FRAS.php';

if (!is_readable($frasPath)) {
    fwrite(STDERR, "error: cannot find the FRAS library at {$frasPath}\n");
    exit(2);
}
require_once $frasPath;
mt_srand($seed);

$letters = array_merge(
    str_split('abcdeijnos'),
    str_split('ABCDEIJNOS'),
    ['é', 'É', 'ü', 'Ü', 'ß'],
    ['α', 'Α', 'σ', 'Σ', 'ς'],
    ['д', 'Д', 'ж', 'Ж'],
    ['İ', 'ı']
);
$delims = [' ', '-', '_', '.', ',', '/', "\t", "\u{00A0}", "\u{3000}", "\u{2009}"];

function randomString(array $letters, array $delims, int $maxLen): string {
    $len = mt_rand(0, $maxLen);
    $s = '';
    for ($i = 0; $i < $len; $i++) {
        $s .= mt_rand(0, 5) === 0 ? $delims[array_rand($delims)] : $letters[array_rand($letters)];
    }
    return $s;
}

// Half the pairs are lower-cased before use, half left mixed, so the
// report can say whether case folding is where any disagreement lives.
$pairs = [];
for ($i = 0; $i < $count; $i++) {
    $q = randomString($letters, $delims, 8);
    $f = randomString($letters, $delims, 16);
    if (mt_rand(0, 3) === 0 && $f !== '') {
        $f = substr_replace($f, $q, 0, 0); // make prefix hits common
    }
    $mixed = $i % 2 === 1;
    if (!$mixed) {
        $q = mb_strtolower($q);
        $f = mb_strtolower($f);
    }
    $pairs[] = [$q, $f, $mixed];
}

// stdin comes from a file rather than a pipe. Thousands of pairs overflow a
// pipe buffer while the probe is still writing, and on Windows a pipe cannot
// be made non-blocking to drain it, so a pipe would deadlock.
$inPath = tempnam(sys_get_temp_dir(), 'fras_diff_');
$in = '';
foreach ($pairs as [$q, $f]) {
    $in .= $q . "\n" . $f . "\n";
}
file_put_contents($inPath, $in);

// Array form: no shell in between.
$proc = proc_open([$probe], [0 => ['file', $inPath, 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']], $pipes);
if (!is_resource($proc)) {
    unlink($inPath);
    fwrite(STDERR, "error: could not run {$probe}\n");
    exit(2);
}
$out = stream_get_contents($pipes[1]);
fclose($pipes[1]);
fclose($pipes[2]);
proc_close($proc);
unlink($inPath);

$rows = array_values(array_filter(explode("\n", str_replace("\r", '', $out)), fn($l) => $l !== ''));
if (count($rows) !== count($pairs)) {
    fwrite(STDERR, sprintf("error: %d pairs sent, %d rows back from the probe\n", count($pairs), count($rows)));
    exit(2);
}

$table = ['lower' => [0, 0], 'mixed' => [0, 0]];
$perComponent = ['prefix' => 0, 'lcs' => 0, 'levenshtein' => 0];
$examples = [];

foreach ($pairs as $i => [$q, $f, $mixed]) {
    preg_match('/prefix=([-\d.]+) lcs=([-\d.]+) lev=([-\d.]+)/', $rows[$i], $m);
    $c = ['prefix' => (float) $m[1], 'lcs' => (float) $m[2], 'levenshtein' => (float) $m[3]];

    $differs = false;
    foreach ([[1.0, 0.0, 0.0, 'prefix'], [0.0, 1.0, 0.0, 'lcs'], [0.0, 0.0, 1.0, 'levenshtein']] as [$wp, $wl, $wv, $name]) {
        $php = (new FRAS($q, [['v' => $f]], $wp, $wl, $wv))->look('v')[0]['similarityScore'];
        if (abs($php - $c[$name]) > 5e-6) {
            $perComponent[$name]++;
            $differs = true;
        }
    }

    $table[$mixed ? 'mixed' : 'lower'][$differs ? 1 : 0]++;
    if ($differs && count($examples) < 5) {
        $examples[] = sprintf('  q=%s f=%s', json_encode($q, JSON_UNESCAPED_UNICODE), json_encode($f, JSON_UNESCAPED_UNICODE));
    }
}

printf("%d random pairs, seed %d\n\n", $count, $seed);
printf("%-26s %6s %6s\n", '', 'agree', 'differ');
printf("%-26s %6d %6d\n", 'pairs already lower-case', ...$table['lower']);
printf("%-26s %6d %6d\n", 'mixed-case pairs', ...$table['mixed']);
printf("\nper component: prefix %d, lcs %d, levenshtein %d\n", $perComponent['prefix'], $perComponent['lcs'], $perComponent['levenshtein']);
if ($examples) {
    echo "\nfirst disagreements:\n", implode("\n", $examples), "\n";
}

exit($table['lower'][1] + $table['mixed'][1] === 0 ? 0 : 1);
