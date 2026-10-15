/*
 * Tests for src/scoring.c -- the half of this project that has to agree
 * with PHP, codepoint for codepoint.
 *
 * Expected values here were taken from the real FRAS::look() (see
 * tools/gen_vectors.php) or, for the UTF-8 decoding cases, from
 * mb_strtolower()/mb_str_split() directly. None of them were derived by
 * reading this project's own code.
 */
#include "harness.h"
#include "scoring.h"

/* Decodes and returns the codepoint count, so length-changing case
 * mappings are visible. */
static size_t folded_len(const char *s) {
    ustr u = ustr_decode_lower(s);
    size_t n = u.len;
    ustr_free(&u);
    return n;
}

/* Returns the k'th codepoint of the folded string, or 0xFFFFFFFF. */
static uint32_t folded_at(const char *s, size_t k) {
    ustr u = ustr_decode_lower(s);
    uint32_t cp = (k < u.len) ? u.cp[k] : 0xFFFFFFFFu;
    ustr_free(&u);
    return cp;
}

static fras_scores score(const char *q, const char *f) {
    ustr uq = ustr_decode_lower(q);
    ustr uf = ustr_decode_lower(f);
    fras_scores s = {-1.0, -1.0, -1.0};
    if (ustr_ok(&uq) && ustr_ok(&uf)) fras_score_pair(&uq, &uf, &s);
    ustr_free(&uq);
    ustr_free(&uf);
    return s;
}

/* ---------------------------------------------------------------- */

static void test_ascii_folding(void) {
    CHECK_INT(folded_at("A", 0), 'a', "ASCII upper folds");
    CHECK_INT(folded_at("Z", 0), 'z', "ASCII upper folds");
    CHECK_INT(folded_at("a", 0), 'a', "ASCII lower unchanged");
    CHECK_INT(folded_at("5", 0), '5', "digit unchanged");
    CHECK_INT(folded_len(""), 0, "empty string folds to nothing");
}

static void test_latin1_folding(void) {
    CHECK_INT(folded_at("\xC3\x89", 0), 0xE9, "U+00C9 folds to U+00E9");
    CHECK_INT(folded_at("\xC3\x9C", 0), 0xFC, "U+00DC folds to U+00FC");
    /* U+00D7 is the multiplication sign sitting inside the uppercase
     * block; adding 32 to it would produce the division sign. */
    CHECK_INT(folded_at("\xC3\x97", 0), 0xD7, "U+00D7 is not a letter and must not shift");
    /* U+00DF is lowercase already and has no uppercase in this direction. */
    CHECK_INT(folded_at("\xC3\x9F", 0), 0xDF, "U+00DF unchanged");
}

/* Regression: folding used to cover only ASCII and Latin-1, so a Greek or
 * Cyrillic query scored a literal prefix of 0 against its own field. */
static void test_folding_beyond_latin1(void) {
    CHECK_INT(folded_at("\xCE\x91", 0), 0x3B1, "U+0391 ALPHA folds to U+03B1");
    CHECK_INT(folded_at("\xD0\x9C", 0), 0x43C, "U+041C EM folds to U+043C");
    CHECK_INT(folded_at("\xD0\x81", 0), 0x451, "U+0401 IO folds to U+0451");
    CHECK_INT(folded_at("\xEF\xBC\xA1", 0), 0xFF41, "fullwidth A folds to fullwidth a");

    fras_scores s = score("\xCE\x91\xCE\x98\xCE\x97\xCE\x9D\xCE\x91",   /* ΑΘΗΝΑ */
                          "\xCE\xB1\xCE\xB8\xCE\xB7\xCE\xBD\xCE\xB1 plaza");
    CHECK_NEAR(s.prefix, 1.0, "upper-case Greek query prefixes its own lower-case field");
}

/* Regression: U+0130 lower-cases to TWO codepoints, which makes the folded
 * string longer than the input. String length is the denominator of both
 * the LCS and Levenshtein scores, so getting this wrong shifts them. */
static void test_dotted_capital_i_expands(void) {
    CHECK_INT(folded_len("\xC4\xB0"), 2, "U+0130 folds to two codepoints");
    CHECK_INT(folded_at("\xC4\xB0", 0), 0x69, "first is LATIN SMALL LETTER I");
    CHECK_INT(folded_at("\xC4\xB0", 1), 0x307, "second is COMBINING DOT ABOVE");
    CHECK_INT(folded_len("\xC4\xB0stanbul"), 9, "\"Istanbul\" with dotted I is 9 codepoints folded");
}

/* Greek final sigma is the one context-sensitive rule mb_strtolower
 * applies, so a per-character table alone gets it wrong. */
static void test_final_sigma(void) {
    /* ΟΔΟΣ -> οδος : the trailing sigma takes the final form. */
    CHECK_INT(folded_at("\xCE\x9F\xCE\x94\xCE\x9F\xCE\xA3", 3), 0x3C2,
              "word-final sigma folds to U+03C2");
    /* ΣΟΦΙΑ -> σοφια : leading sigma has no preceding cased letter. */
    CHECK_INT(folded_at("\xCE\xA3\xCE\x9F\xCE\xA6\xCE\x99\xCE\x91", 0), 0x3C3,
              "word-initial sigma folds to U+03C3");
    /* A lone sigma likewise has nothing before it. */
    CHECK_INT(folded_at("\xCE\xA3", 0), 0x3C3, "isolated sigma folds to U+03C3");
    /* ΑΣΣ -> ασς : only the last one is final. */
    CHECK_INT(folded_at("\xCE\x91\xCE\xA3\xCE\xA3", 1), 0x3C3, "medial sigma is not final");
    CHECK_INT(folded_at("\xCE\x91\xCE\xA3\xCE\xA3", 2), 0x3C2, "trailing sigma is final");
}

/* Regression: undecodable bytes become mbstring's substitute character
 * '?', and one substitute covers a whole maximal invalid subpart rather
 * than one per byte. Both halves change the folded length. */
static void test_invalid_utf8(void) {
    CHECK_INT(folded_len("a\x80" "b"), 3, "stray continuation byte is one substitute");
    CHECK_INT(folded_at("a\x80" "b", 1), '?', "substitute is '?', not U+FFFD");

    /* C0 is never a valid lead byte and AF is a stray continuation: two. */
    CHECK_INT(folded_len("a\xC0\xAF" "b"), 4, "overlong pair is two substitutes");

    /* E2 82 is a valid *prefix* of a 3-byte sequence, so the two bytes
     * collapse into a single substitute. This is the case that a
     * one-substitute-per-byte decoder gets wrong. */
    CHECK_INT(folded_len("a\xE2\x82" "b"), 3, "truncated 3-byte sequence is ONE substitute");

    /* ED A0 80 would be a surrogate: rejected at the lead byte, so the
     * two continuations are counted separately. */
    CHECK_INT(folded_len("a\xED\xA0\x80" "b"), 5, "surrogate encoding is three substitutes");

    /* F4 90 .. would be above U+10FFFF. */
    CHECK_INT(folded_len("a\xF4\x90\x80\x80" "b"), 6, "out-of-range 4-byte is four substitutes");

    /* A valid sequence next to an invalid one still decodes. */
    CHECK_INT(folded_len("\xC3\xA9\x80"), 2, "valid sequence survives a following bad byte");
}

/* Regression: the delimiter set used to be ASCII-only, so a field split on
 * a non-breaking space produced one token where PHP's /u regex made two --
 * so the second word was never seen as a word start. */
static void test_token_delimiters(void) {
    /* "corp" matches the start of token 1 -> (4/4) * 1/(1+1) = 0.5 */
    CHECK_NEAR(score("corp", "acme-corp").prefix, 0.5, "hyphen splits tokens");
    CHECK_NEAR(score("corp", "acme_corp").prefix, 0.5, "underscore splits tokens");
    CHECK_NEAR(score("corp", "acme,corp").prefix, 0.5, "comma splits tokens");
    CHECK_NEAR(score("corp", "acme.corp").prefix, 0.5, "full stop splits tokens");
    CHECK_NEAR(score("corp", "acme/corp").prefix, 0.5, "solidus splits tokens");
    CHECK_NEAR(score("corp", "acme\tcorp").prefix, 0.5, "tab splits tokens");
    CHECK_NEAR(score("corp", "acme\vcorp").prefix, 0.5, "vertical tab splits tokens");
    CHECK_NEAR(score("corp", "acme\fcorp").prefix, 0.5, "form feed splits tokens");
    CHECK_NEAR(score("corp", "acme\xC2\xA0" "corp").prefix, 0.5, "no-break space splits tokens");
    CHECK_NEAR(score("corp", "acme\xE3\x80\x80" "corp").prefix, 0.5, "ideographic space splits tokens");
    CHECK_NEAR(score("corp", "acme\xE2\x80\x89" "corp").prefix, 0.5, "thin space splits tokens");
    CHECK_NEAR(score("corp", "acme\xC2\x85" "corp").prefix, 0.5, "next-line splits tokens");

    /* Runs of delimiters collapse and leading ones produce no empty token,
     * matching PREG_SPLIT_NO_EMPTY -- so "corp" is still token 1. */
    CHECK_NEAR(score("corp", "---acme---corp---").prefix, 0.5, "delimiter runs collapse");

    /* A character that is not a delimiter must not split. */
    CHECK_NEAR(score("corp", "acme+corp").prefix, 0.0, "plus is not a delimiter");
}

static void test_prefix_and_token_bonus(void) {
    /* Literal prefix wins outright. */
    CHECK_NEAR(score("john", "john smith").prefix, 1.0, "full literal prefix");
    CHECK_NEAR(score("joh", "john smith").prefix, 1.0, "query shorter than field");
    CHECK_NEAR(score("jo", "jx").prefix, 0.5, "partial literal prefix");
    CHECK_NEAR(score("", "abc").prefix, 0.0, "empty query scores zero prefix");

    /* Token bonus decays with token index: token 1 -> 1/2, token 3 -> 1/4. */
    CHECK_NEAR(score("b", "a b").prefix, 0.5, "token 1 bonus is halved");
    CHECK_NEAR(score("d", "a b c d").prefix, 0.25, "token 3 bonus is quartered");
    /* The best token wins, and earlier tokens decay less. */
    CHECK_NEAR(score("b", "a b c b").prefix, 0.5, "earliest matching token wins");
    /* A match on token 0 is not double-counted: literal and bonus agree. */
    CHECK_NEAR(score("acme", "acme corp").prefix, 1.0, "token 0 handled by literal prefix");

    /* Regression: token 0 used to be skipped by the bonus on the grounds
     * that the literal prefix covers it, which is only true when the field
     * starts with it. After a leading delimiter neither check saw the first
     * word. FRAS::look() had the same skip and was fixed the same way. */
    CHECK_NEAR(score("smith", " smith").prefix, 1.0, "first word after a leading space");
    CHECK_NEAR(score("net", ".net framework").prefix, 1.0, "first word after a leading full stop");
    CHECK_NEAR(score("acme", "---acme---corp---").prefix, 1.0, "first word after a delimiter run");
    CHECK_NEAR(score("corp", "\xE3\x80\x80" "corp").prefix, 1.0, "first word after an ideographic space");
}

static void test_lcs_and_levenshtein(void) {
    /* Identical strings. */
    CHECK_NEAR(score("abc", "abc").lcs, 1.0, "identical strings: lcs 1");
    CHECK_NEAR(score("abc", "abc").levenshtein, 1.0, "identical strings: lev 1");

    /* No shared characters: the lossless short-circuit path. */
    CHECK_NEAR(score("abc", "xyz").lcs, 0.0, "disjoint strings: lcs 0");
    CHECK_NEAR(score("abc", "xyz").levenshtein, 0.0, "disjoint equal-length: lev 0");

    /* Normalisation denominators: lcs over min(len), lev over max(len). */
    CHECK_NEAR(score("ab", "ba").lcs, 0.5, "lcs normalised by the shorter length");
    CHECK_NEAR(score("a", "aaaa").lcs, 1.0, "lcs of 1 over min length 1");
    CHECK_NEAR(score("a", "aaaa").levenshtein, 0.25, "lev distance 3 over max length 4");

    /* Empty strings: PHP returns 0 for lcs and 1 for lev when both are
     * empty, because maxLen is 0. */
    CHECK_NEAR(score("", "abc").lcs, 0.0, "empty query: lcs 0");
    CHECK_NEAR(score("", "abc").levenshtein, 0.0, "empty query against abc: lev 0");
    CHECK_NEAR(score("", "").lcs, 0.0, "both empty: lcs 0");
    CHECK_NEAR(score("", "").levenshtein, 1.0, "both empty: lev 1");

    /* Order independence: swapping the arguments must not change either. */
    CHECK_NEAR(score("abcd", "ab").lcs, score("ab", "abcd").lcs, "lcs is symmetric");
    CHECK_NEAR(score("abcd", "ab").levenshtein, score("ab", "abcd").levenshtein, "lev is symmetric");
}

/* The DP is a rolling two-row buffer with a swap; a long field exercises
 * more iterations than a short one and catches a stale row. */
static void test_dp_over_longer_strings(void) {
    fras_scores s = score("kitten", "sitting");
    /* Levenshtein("kitten","sitting") = 3, max length 7. */
    CHECK_NEAR(s.levenshtein, 1.0 - 3.0 / 7.0, "classic kitten/sitting distance");
    /* LCS("kitten","sitting") = "ittn" = 4, min length 6. */
    CHECK_NEAR(s.lcs, 4.0 / 6.0, "classic kitten/sitting LCS");

    fras_scores t = score("the quick brown fox", "the quick brown fox jumps over");
    CHECK_NEAR(t.lcs, 1.0, "prefix of a longer string has full LCS");
    CHECK_NEAR(t.levenshtein, 1.0 - 11.0 / 30.0, "distance is the tail length");
}

void run_scoring_tests(void) {
    test_ascii_folding();
    test_latin1_folding();
    test_folding_beyond_latin1();
    test_dotted_capital_i_expands();
    test_final_sigma();
    test_invalid_utf8();
    test_token_delimiters();
    test_prefix_and_token_bonus();
    test_lcs_and_levenshtein();
    test_dp_over_longer_strings();
}
