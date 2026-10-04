CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
SRC      = rx.c
TESTS    = tests

.PHONY: all test cli torture asan ubsan fault valgrind cov clean

VALGRIND ?= valgrind --leak-check=full --show-leak-kinds=all \
    --errors-for-leak-kinds=all --error-exitcode=1 -q

all: cli

# Sed-like command-line tool: rsed [-gims] PATTERN REPLACEMENT < input
cli: rsed

rsed: $(SRC) rx.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DRX_MAIN $(SRC) -o $@

# Build and run the fast public-API self-test.
test: $(TESTS)/selftest
	./$(TESTS)/selftest

$(TESTS)/selftest: $(TESTS)/selftest.c $(SRC) rx.h
	$(CC) $(CFLAGS) $(TESTS)/selftest.c -o $@

# A smaller per-search step budget keeps pathological fuzz patterns from
# dominating the run time while still exercising the abort path.
TORTURE_CFLAGS = -DRX_STEP_LIMIT=2000000

# The torture suite lives in tests/ and includes ../rx.c directly so the
# sanitizers and gcov see the whole engine as one translation unit.
TORTURE_SRC = $(TESTS)/torture.c

# Build and run the heavy torture + fuzz suite.
torture: $(TESTS)/torturet
	./$(TESTS)/torturet

$(TESTS)/torturet: $(TORTURE_SRC) $(SRC) rx.h
	$(CC) $(CFLAGS) $(TORTURE_CFLAGS) $(TORTURE_SRC) -o $@

# Torture suite under AddressSanitizer (plus leak detection).
asan: $(TORTURE_SRC) $(SRC) rx.h
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    -fsanitize=address -fno-omit-frame-pointer $(TORTURE_SRC) \
	    -o $(TESTS)/torture-asan
	./$(TESTS)/torture-asan 5000

# Torture suite under UndefinedBehaviorSanitizer.
ubsan: $(TORTURE_SRC) $(SRC) rx.h
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    -fsanitize=undefined -fno-sanitize-recover=all \
	    -fno-omit-frame-pointer $(TORTURE_SRC) -o $(TESTS)/torture-ubsan
	./$(TESTS)/torture-ubsan 5000

# Exhaustive allocation-failure sweep under AddressSanitizer. Every
# allocation in the engine is failed in turn; the leak detector proves
# each error path releases its partial state. The "0" argument skips the
# fuzzer so this target is just the sweep.
fault: $(TORTURE_SRC) $(SRC) rx.h
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    -fsanitize=address -fno-omit-frame-pointer $(TORTURE_SRC) \
	    -o $(TESTS)/torture-fault
	./$(TESTS)/torture-fault 0

# Allocation-failure sweep under valgrind memcheck, an independent check
# on the error-path cleanup (uninstrumented build; the fuzzer is skipped).
valgrind: $(TORTURE_SRC) $(SRC) rx.h
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    $(TORTURE_SRC) -o $(TESTS)/torture-vg
	$(VALGRIND) ./$(TESTS)/torture-vg 0

# Line coverage of the engine from the torture suite. Built and run inside
# tests/ so the coverage artifacts stay out of the top level.
cov: $(TORTURE_SRC) $(SRC) rx.h
	cd $(TESTS) && $(CC) -std=c99 -O0 -g $(TORTURE_CFLAGS) --coverage \
	    torture.c -o torture-cov && ./torture-cov && \
	    gcov torture-cov-torture >/dev/null 2>&1 || true
	@echo "see $(TESTS)/rx.c.gcov for per-line counts"

clean:
	rm -f rsed $(TESTS)/selftest $(TESTS)/torturet $(TESTS)/torture-asan \
	    $(TESTS)/torture-ubsan $(TESTS)/torture-fault $(TESTS)/torture-vg \
	    $(TESTS)/torture-cov $(TESTS)/*.gcno $(TESTS)/*.gcda $(TESTS)/*.gcov
