<?php
/*
 * Generates tests/vectors.json: the shared (query, field) -> components
 * expectations that pin this trainer's scoring to FRAS::look()'s.
 *
 * Expected values come from driving the real PHP class, not from a second
 * description of what it ought to do. FRAS::look() does not expose the
 * three components directly, so each is extracted by running the pair
 * through it three times with the weights [1,0,0], [0,1,0] and [0,0,1] --
 * similarityScore is then exactly that one component.
 *
 * Vectors are split into two groups:
 *
 *   "agreed"    - pairs where C and PHP produce the same three numbers.
 *                 Both sides assert these. This is the contract.
 *   "divergent" - pairs where they do not, each recording both answers and
 *                 why. tests/README.md explains the single cause. The C
 *                 suite asserts the "c" values and the PHP checker asserts
 *                 the "php" values, so whichever side changes first, a test
 *                 fails and says so.
 *
 * Run: php -d extension=mbstring tools/gen_vectors.php <path-to-cprobe.exe>
 *
 * where cprobe is built by tests/build (it prints this project's three
 * components for pairs fed to it on stdin).
 */

$frasPath = __DIR__ . '/../../FRAS Algorithm/src/FRAS.php';
if (!is_readable($frasPath)) {
    fwrite(STDERR, "error: cannot find the FRAS library at {$frasPath}\n");
    fwrite(STDERR, "       this generator needs the sibling project checked out beside this one\n");
    exit(1);
}
require_once $frasPath;

if ($argc < 3) {
    fwrite(STDERR, "usage: php tools/gen_vectors.php <path-to-probe executable> <output.json>\n");
    fwrite(STDERR, "\n");
    fwrite(STDERR, "The output path is an argument rather than a shell redirect because\n");
    fwrite(STDERR, "PowerShell's '>' writes a UTF-8 BOM, and a BOM in a generated file is\n");
    fwrite(STDERR, "invisible in every diff that would have caught it.\n");
    exit(1);
}
$cprobe = $argv[1];
$outPath = $argv[2];

/* --- the pairs ---------------------------------------------------------
 * Chosen to cover each thing that can move a score: case in both
 * directions, scripts outside Latin-1, the Latin-1 block itself, the
 * length-changing dotted I, Greek final sigma, every token delimiter
 * including the non-ASCII ones, empty strings, and invalid UTF-8. */
$pairs = [
    // plain ASCII, no case involved
    ['john', 'john smith'],
    ['abc', 'abc def'],
    ['ab', 'a b'],
    ['x', 'a-b-c-x'],
    ['free', 'free text search here'],
    ['a', 'aaaa'],
    ['aaaa', 'a'],
    ['ab', 'ba'],
    ['ab', 'ba xy'],
    ['', 'abc'],
    ['abc', ''],
    ['', ''],
    // token delimiters, one per delimiter class
    ['corp', 'acme-corp'],
    ['corp', 'acme_corp'],
    ['corp', 'acme,corp'],
    ['corp', 'acme.corp'],
    ['corp', 'acme/corp'],
    ['corp', "acme\tcorp"],
    ['corp', "acme\ncorp"],
    ['corp', "acme\u{00A0}corp"],   // no-break space
    ['corp', "acme\u{3000}corp"],   // ideographic space
    ['corp', "acme\u{2009}corp"],   // thin space
    ['corp', '---acme---corp---'],  // leading/repeated delimiters
    ['b', 'a b c b'],               // decay picks the earlier token
    // the first word after a leading delimiter is token 0 but not the
    // literal prefix; both sides used to skip it
    ['smith', ' smith'],
    ['net', '.net framework'],
    ['acme', '---acme---corp---'],
    ['corp', "\u{3000}corp"],
    // Latin-1 supplement
    ['café', 'café royal'],
    ['müller', 'müller gmbh'],
    ['strasse', 'hauptstraße 5'],
    // scripts the old Latin-1-only folding could not reach
    ['αθηνα', 'αθηνα plaza'],
    ['москва', 'москва центр'],
    ['αβγ', 'αβγδε'],
    // Greek final sigma: same letters, different position
    ['οδος', 'οδος αθηνας'],
    ['σοφια', 'σοφια παππα'],
    // the length-changing mapping
    ['istanbul', 'istanbul hotel'],
    ['i̇stanbul', 'i̇stanbul hotel'],
    // Invalid UTF-8 is deliberately NOT here: it cannot be written into a
    // JSON file without being mangled by the encoder, which would make the
    // vectors lie about what was tested. tests/scoring_test.c covers it
    // directly with byte literals instead.
    //
    // mixed case: these are the ones that expose the divergence
    ['john', 'John Smith'],
    ['JOHN', 'john smith'],
    ['smith', 'John Smith'],
    ['ACME', 'acme-corp'],
    ['cafe', 'CAFÉ Royal'],
    ['sku123', 'SKU-123-X'],
    ['ΑΘΗΝΑ', 'αθηνα plaza'],
    ['ΟΔΟΣ', 'ΟΔΟΣ ΑΘΗΝΑΣ'],
    ['istanbul', 'İstanbul Hotel'],
];

/* --- PHP side ---------------------------------------------------------- */

function phpComponents(string $q, string $f): array {
    $out = [];
    foreach ([[1, 0, 0], [0, 1, 0], [0, 0, 1]] as $w) {
        $fras = new FRAS($q, [['v' => $f]], (float) $w[0], (float) $w[1], (float) $w[2]);
        $r = $fras->look('v');
        $out[] = round($r[0]['similarityScore'], 12);
    }
    return ['prefix' => $out[0], 'lcs' => $out[1], 'levenshtein' => $out[2]];
}

/* --- C side ------------------------------------------------------------ */

$stdin = '';
foreach ($pairs as [$q, $f]) {
    if (strpos($q, "\n") !== false || strpos($f, "\n") !== false) {
        // The probe's protocol is line-based; encode such pairs as escapes.
        continue;
    }
    $stdin .= $q . "\n" . $f . "\n";
}

$desc = [0 => ['pipe', 'r'], 1 => ['pipe', 'w'], 2 => ['pipe', 'w']];
$proc = proc_open($cprobe, $desc, $pipes);
if (!is_resource($proc)) {
    fwrite(STDERR, "error: could not run {$cprobe}\n");
    exit(1);
}
fwrite($pipes[0], $stdin);
fclose($pipes[0]);
$cOut = stream_get_contents($pipes[1]);
fclose($pipes[1]);
fclose($pipes[2]);
proc_close($proc);

$cLines = array_values(array_filter(explode("\n", trim($cOut)), fn($l) => $l !== ''));

/* --- merge ------------------------------------------------------------- */

$agreed = [];
$divergent = [];
$li = 0;

foreach ($pairs as [$q, $f]) {
    if (strpos($q, "\n") !== false || strpos($f, "\n") !== false) continue;
    if (!isset($cLines[$li])) {
        fwrite(STDERR, "error: the C probe produced fewer rows than there are pairs\n");
        exit(1);
    }
    if (!preg_match('/prefix=([-\d.]+) lcs=([-\d.]+) lev=([-\d.]+)/', $cLines[$li], $m)) {
        fwrite(STDERR, "error: unparsable probe output: {$cLines[$li]}\n");
        exit(1);
    }
    $li++;

    $c = ['prefix' => (float) $m[1], 'lcs' => (float) $m[2], 'levenshtein' => (float) $m[3]];
    $p = phpComponents($q, $f);

    $same = true;
    foreach (['prefix', 'lcs', 'levenshtein'] as $k) {
        if (abs($c[$k] - $p[$k]) > 1e-6) $same = false;
    }

    $entry = ['query' => $q, 'field' => $f];
    if ($same) {
        $agreed[] = $entry + ['expected' => array_map(fn($v) => round($v, 6), $c)];
    } else {
        $divergent[] = $entry + [
            'c' => array_map(fn($v) => round($v, 6), $c),
            'php' => array_map(fn($v) => round($v, 6), $p),
        ];
    }
}

$doc = [
    'generated_by' => 'tools/gen_vectors.php',
    'php_version' => PHP_VERSION,
    'note' => 'Expected values are produced by running the real FRAS::look() '
        . 'with weights [1,0,0], [0,1,0] and [0,0,1]. See tests/README.md.',
    'divergence_cause' => $divergent === []
        ? 'None: every pair agrees.'
        : 'Unexplained. Every entry under "divergent" is a pair on which this '
            . 'trainer and FRAS::look() disagree; find out why before fitting '
            . 'weights. See tests/README.md.',
    'agreed' => $agreed,
    'divergent' => $divergent,
];

$json = json_encode($doc, JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
if ($json === false) {
    fwrite(STDERR, "error: could not encode vectors: " . json_last_error_msg() . "\n");
    exit(1);
}
file_put_contents($outPath, $json . "\n");

fwrite(STDERR, sprintf("wrote %s -- agreed: %d, divergent: %d\n", $outPath, count($agreed), count($divergent)));
