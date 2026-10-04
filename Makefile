CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
SRC      = regex.c

.PHONY: all test cli torture asan ubsan fault cov clean

all: cli

# Sed-like command-line tool: rsed [-gims] PATTERN REPLACEMENT < input
cli: rsed

rsed: $(SRC)
	$(CC) $(CFLAGS) -D_GNU_SOURCE -DRX_MAIN $(SRC) -o $@

# Build and run the in-file self-test harness.
test: rxtest
	./rxtest

rxtest: $(SRC)
	$(CC) $(CFLAGS) -DRX_TEST $(SRC) -o $@

# A smaller per-search step budget keeps pathological fuzz patterns from
# dominating the run time while still exercising the abort path.
TORTURE_CFLAGS = -DRX_STEP_LIMIT=2000000

# Build and run the heavy torture + fuzz suite. torture.c includes
# regex.c directly.
torture: torturet
	./torturet

torturet: torture.c $(SRC)
	$(CC) $(CFLAGS) $(TORTURE_CFLAGS) torture.c -o $@

# Torture suite under AddressSanitizer (plus leak detection).
asan: torture.c $(SRC)
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    -fsanitize=address -fno-omit-frame-pointer torture.c -o torture-asan
	./torture-asan 5000

# Torture suite under UndefinedBehaviorSanitizer.
ubsan: torture.c $(SRC)
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    -fsanitize=undefined -fno-sanitize-recover=all \
	    -fno-omit-frame-pointer torture.c -o torture-ubsan
	./torture-ubsan 5000

# Exhaustive allocation-failure sweep under AddressSanitizer. Every
# allocation in the engine is failed in turn; the leak detector proves
# each error path releases its partial state. The "0" argument skips the
# fuzzer so this target is just the sweep.
fault: torture.c $(SRC)
	$(CC) -std=c99 -Wall -Wextra -g -O1 $(TORTURE_CFLAGS) \
	    -fsanitize=address -fno-omit-frame-pointer torture.c -o torture-fault
	./torture-fault 0

# Line coverage of the engine from the torture suite.
cov: torture.c $(SRC)
	$(CC) -std=c99 -O0 -g $(TORTURE_CFLAGS) --coverage torture.c -o torture-cov
	./torture-cov
	gcov torture-cov-torture >/dev/null 2>&1 || true
	@echo "see regex.c.gcov for per-line counts"

clean:
	rm -f rsed rxtest torturet torture-asan torture-ubsan torture-fault \
	    torture-cov *.gcno *.gcda *.gcov
