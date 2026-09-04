CC = gcc
CFLAGS = -I. -I../.. -I../../toxcore -I../ -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-variable -g -O1
LDFLAGS = -lsodium -lpthread -ldl

MID_SRC = ../mid_roster.c
MOCK_SRC = tox_mocks.c

# Automatically find every test_*.c file in this directory.
TEST_SRCS := $(sort $(wildcard test_*.c))

# Binary name = source file name without .c
# Example: test_midroster_unit.c -> test_midroster_unit
TEST_BINS := $(TEST_SRCS:.c=)

GRAND_TMP = .test_grand_total.tmp
GRAND_LABEL ?= Normal
SAN_FLAGS ?=

.PHONY: all asan ubsan tsan clean run_all_bins build

all:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory build SAN_FLAGS=""
	@$(MAKE) --no-print-directory run_all_bins GRAND_LABEL="Normal"

asan:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory build SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
	@$(MAKE) --no-print-directory run_all_bins GRAND_LABEL="ASan"

ubsan:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory build SAN_FLAGS="-fsanitize=undefined -fno-omit-frame-pointer"
	@$(MAKE) --no-print-directory run_all_bins GRAND_LABEL="UBSan"

tsan:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory build SAN_FLAGS="-fsanitize=thread -fno-omit-frame-pointer"
	@$(MAKE) --no-print-directory run_all_bins GRAND_LABEL="TSan"

build: $(TEST_BINS)

# Automatic rule:
#   test_foo.c -> test_foo
#
# Every test is linked with:
#   - the middleware source
#   - the shared mock file
#
# If a test defines its own mock functions, those will override the weak
# mock functions in tox_mocks.c.
$(TEST_BINS): %: %.c $(MID_SRC) $(MOCK_SRC)
	$(CC) $(CFLAGS) $(SAN_FLAGS) -o $@ $< $(MID_SRC) $(MOCK_SRC) $(LDFLAGS) $(SAN_FLAGS)

run_all_bins:
	@rm -f $(GRAND_TMP)
	@touch $(GRAND_TMP)
	@for bin in $(TEST_BINS); do \
		echo ""; \
		echo "==== Running $$bin ===="; \
		TEST_GRAND_FILE=$(GRAND_TMP) ./$$bin || true; \
	done
	@echo ""
	@awk -v label="$(GRAND_LABEL)" '{ total += $$1; passed += $$2; failed += $$3; suites += $$4; suites_failed += $$5 } END { printf "============================================================\n"; printf "  GRAND TOTAL (%s)\n", label; printf "============================================================\n"; printf "    Individual tests:  %d total, %d passed, %d failed\n", total, passed, failed; printf "    Suites:            %d run, %d failed\n", suites, suites_failed; printf "============================================================\n"; printf "  RESULT: %s\n", ((failed == 0 && suites_failed == 0) ? "PASS" : "FAIL"); exit ((failed == 0 && suites_failed == 0) ? 0 : 1) }' $(GRAND_TMP)

clean:
	rm -f $(TEST_BINS) $(GRAND_TMP)
