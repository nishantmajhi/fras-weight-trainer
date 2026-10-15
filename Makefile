CC      ?= cc
CFLAGS  ?= -std=c11 -O3 -Wall -Wextra -Iinclude -Isrc
LDFLAGS ?= -lpthread -lm

SRC := src/json.c src/scoring.c src/fit.c src/main.c
OBJ := $(SRC:.c=.o)
BIN := fras-weight-trainer

TEST_SRC := tests/run.c tests/scoring_test.c tests/json_test.c \
            tests/fit_test.c tests/vectors_test.c

.PHONY: all clean test check tables vectors

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

src/scoring.o: src/unicode_tables.h

# The suite. src/json.c is compiled a second time with its allocation
# hooks redirected into the test binary, which is what lets json_test.c
# fail an allocation on demand -- see tests/README.md.
test:
	@mkdir -p tests/build
	$(CC) $(CFLAGS) -o tests/build/probe tests/probe.c src/scoring.c $(LDFLAGS)
	$(CC) $(CFLAGS) -include tests/alloc_hooks.h \
	    -DJSON_MALLOC=json_test_malloc \
	    -DJSON_CALLOC=json_test_calloc \
	    -DJSON_REALLOC=json_test_realloc \
	    -c -o tests/build/json_hooked.o src/json.c
	$(CC) $(CFLAGS) -c -o tests/build/scoring.o src/scoring.c
	$(CC) $(CFLAGS) -c -o tests/build/fit.o src/fit.c
	$(CC) $(CFLAGS) -Itests -o tests/build/run $(TEST_SRC) \
	    tests/build/json_hooked.o tests/build/scoring.o tests/build/fit.o $(LDFLAGS)
	./tests/build/run tests/vectors.json

# Replays tests/vectors.json through the real PHP library next door.
# Needs php with ext-mbstring and the sibling FRAS Algorithm checkout.
check: test
	php tools/check_php.php tests/vectors.json

# Regenerating either of these needs PHP; both outputs are committed so a
# normal build and a normal test run do not.
tables:
	php tools/gen_unicode_tables.php src/unicode_tables.h

vectors:
	@mkdir -p tests/build
	$(CC) $(CFLAGS) -o tests/build/probe tests/probe.c src/scoring.c $(LDFLAGS)
	php tools/gen_vectors.php tests/build/probe tests/vectors.json

clean:
	rm -f $(OBJ) $(BIN)
	rm -rf tests/build
