SHELL      := /bin/bash
CC         ?= gcc

# ── Paths ─────────────────────────────────────────────────────────
AMALGAMATION_C := ../amalgamation/toxcore_amalgamation_no_toxav.c
BUILD_DIR      := _build
BUILD_DIR_ASAN := _build_asan

# ── Compiler flags ───────────────────────────────────────────────
CFLAGS     := -std=gnu11 -D_GNU_SOURCE -DTOX_HAVE_TOXUTIL -Wall -Wextra -g -O0 \
              -I. -I../amalgamation -I../toxcore -I../toxutil -Wno-unused-parameter
CFLAGS     += $(shell pkg-config --cflags libsodium)

# ── Linker flags ─────────────────────────────────────────────────
LDLIBS     := $(shell pkg-config --libs libsodium) -lpthread

# ── AddressSanitizer flags ───────────────────────────────────────
# -fno-omit-frame-pointer  → readable stack traces
# -O1 -g                   → better detection + accurate line info
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer -O1 -g

# ── Test binaries ────────────────────────────────────────────────
TEST_SRCS  := $(wildcard test_*.c)
ALL_BINS   := $(TEST_SRCS:.c=)
ASAN_BINS  := $(addsuffix _asan,$(ALL_BINS))

.PHONY: all test test_asan asan clean clean_asan objs objs_asan

all: $(ALL_BINS)

# ── Normal build: shared objects ─────────────────────────────────
objs: $(BUILD_DIR)/toxcore.o

$(BUILD_DIR)/toxcore.o: $(AMALGAMATION_C) | $(BUILD_DIR)
	@echo "=== Compiling Toxcore Amalgamation (no ToxAV) ==="
	$(CC) -c $(CFLAGS) -O2 -fPIC -Wno-discarded-qualifiers $(AMALGAMATION_C) -o $@

# ── Normal build: test binaries ──────────────────────────────────
test_%: test_%.c test_framework.h objs
	@echo "=== Building $@ ==="
	$(CC) $(CFLAGS) $< $(BUILD_DIR)/toxcore.o -o $@ $(LDLIBS)

# ── ASAN build: shared objects ───────────────────────────────────
objs_asan: $(BUILD_DIR_ASAN)/toxcore.o

$(BUILD_DIR_ASAN)/toxcore.o: $(AMALGAMATION_C) | $(BUILD_DIR_ASAN)
	@echo "=== Compiling Toxcore Amalgamation with ASAN ==="
	$(CC) -c $(CFLAGS) $(ASAN_FLAGS) -fPIC -Wno-discarded-qualifiers $(AMALGAMATION_C) -o $@

# ── ASAN build: test binaries ────────────────────────────────────
test_%_asan: test_%.c test_framework.h objs_asan
	@echo "=== Building $@ (ASAN) ==="
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< $(BUILD_DIR_ASAN)/toxcore.o -o $@ $(LDLIBS)

# ── Run ALL tests (normal) ───────────────────────────────────────
test: $(ALL_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING ALL TEST SUITES"
	@echo "============================================================"
	@GT=0; GP=0; GF=0; SUITES_RUN=0; SUITES_FAILED=0; \
	for BIN in $(ALL_BINS); do \
		echo ""; \
		OUT=$$(mktemp); \
		./$$BIN > "$$OUT" 2>&1; \
		RC=$$?; \
		cat "$$OUT"; \
		SUITES_RUN=$$((SUITES_RUN + 1)); \
		if [ $$RC -ne 0 ]; then \
			SUITES_FAILED=$$((SUITES_FAILED + 1)); \
			echo ""; \
			echo "    >>> SUITE '$$BIN' FAILED (exit code $$RC) <<<"; \
		fi; \
		T=$$(sed 's/\x1b\[[0-9;]*m//g' "$$OUT" | awk '/^  Total:/  {print $$2}' | tail -n1); \
		P=$$(sed 's/\x1b\[[0-9;]*m//g' "$$OUT" | awk '/^  Passed:/ {print $$2}' | tail -n1); \
		F=$$(sed 's/\x1b\[[0-9;]*m//g' "$$OUT" | awk '/^  Failed:/ {print $$2}' | tail -n1); \
		GT=$$((GT + $${T:-0})); \
		GP=$$((GP + $${P:-0})); \
		GF=$$((GF + $${F:-0})); \
		rm -f "$$OUT"; \
	done; \
	echo ""; \
	echo "============================================================"; \
	echo "  GRAND TOTAL"; \
	echo "============================================================"; \
	echo "    Individual tests:  $$GT total, $$GP passed, $$GF failed"; \
	echo "    Suites:            $$SUITES_RUN run, $$SUITES_FAILED failed"; \
	echo "============================================================"; \
	if [ $$SUITES_FAILED -ne 0 ]; then \
		echo "  RESULT: FAIL"; \
		exit 1; \
	else \
		echo "  RESULT: PASS"; \
		exit 0; \
	fi

# ── Run ALL tests under AddressSanitizer ─────────────────────────
test_asan: $(ASAN_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING ALL TEST SUITES UNDER AddressSanitizer"
	@echo "============================================================"
	@GT=0; GP=0; GF=0; SUITES_RUN=0; SUITES_FAILED=0; \
	for BIN in $(ASAN_BINS); do \
		echo ""; \
		echo "---- $$BIN ----"; \
		OUT=$$(mktemp); \
		ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
			./$$BIN > "$$OUT" 2>&1; \
		RC=$$?; \
		cat "$$OUT"; \
		SUITES_RUN=$$((SUITES_RUN + 1)); \
		if [ $$RC -ne 0 ]; then \
			SUITES_FAILED=$$((SUITES_FAILED + 1)); \
			echo ""; \
			echo "    >>> ASAN SUITE '$$BIN' REPORTED MEMORY ERROR (exit code $$RC) <<<"; \
		fi; \
		T=$$(sed 's/\x1b\[[0-9;]*m//g' "$$OUT" | awk '/^  Total:/  {print $$2}' | tail -n1); \
		P=$$(sed 's/\x1b\[[0-9;]*m//g' "$$OUT" | awk '/^  Passed:/ {print $$2}' | tail -n1); \
		F=$$(sed 's/\x1b\[[0-9;]*m//g' "$$OUT" | awk '/^  Failed:/ {print $$2}' | tail -n1); \
		GT=$$((GT + $${T:-0})); \
		GP=$$((GP + $${P:-0})); \
		GF=$$((GF + $${F:-0})); \
		rm -f "$$OUT"; \
	done; \
	echo ""; \
	echo "============================================================"; \
	echo "  GRAND TOTAL (ASAN)"; \
	echo "============================================================"; \
	echo "    Individual tests:  $$GT total, $$GP passed, $$GF failed"; \
	echo "    Suites:            $$SUITES_RUN run, $$SUITES_FAILED failed"; \
	echo "============================================================"; \
	if [ $$SUITES_FAILED -ne 0 ]; then \
		echo "  RESULT: FAIL (memory errors detected)"; \
		exit 1; \
	else \
		echo "  RESULT: PASS (no memory errors)"; \
		exit 0; \
	fi

# ── Combined ASan + UBSan build ──────────────────────────────────
BUILD_DIR_SAN := _build_san
SAN_FLAGS     := -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g \
                 -fno-sanitize-recover=all

SAN_BINS      := $(addsuffix _san,$(ALL_BINS))

objs_san: $(BUILD_DIR_SAN)/toxcore.o

$(BUILD_DIR_SAN)/toxcore.o: $(AMALGAMATION_C) | $(BUILD_DIR_SAN)
	@echo "=== Compiling Toxcore Amalgamation with ASan+UBSan ==="
	$(CC) -c $(CFLAGS) $(SAN_FLAGS) -fPIC -Wno-discarded-qualifiers $(AMALGAMATION_C) -o $@

test_%_san: test_%.c test_framework.h objs_san
	@echo "=== Building $@ (ASan+UBSan) ==="
	$(CC) $(CFLAGS) $(SAN_FLAGS) $< $(BUILD_DIR_SAN)/toxcore.o -o $@ $(LDLIBS)

test_san: $(SAN_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING ALL SUITES UNDER ASan + UBSan"
	@echo "============================================================"
	@SUITES_FAILED=0; SUITES_RUN=0; \
	for BIN in $(SAN_BINS); do \
		echo ""; echo "---- $$BIN ----"; \
		OUT=$$(mktemp); \
		ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
		UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
			./$$BIN > "$$OUT" 2>&1; \
		RC=$$?; \
		cat "$$OUT"; \
		SUITES_RUN=$$((SUITES_RUN+1)); \
		if [ $$RC -ne 0 ]; then \
			SUITES_FAILED=$$((SUITES_FAILED+1)); \
			echo "    >>> SANITIZER SUITE '$$BIN' FAILED (exit $$RC) <<<"; \
		fi; \
		rm -f "$$OUT"; \
	done; \
	echo ""; \
	echo "============================================================"; \
	echo "  Sanitizer suites: $$SUITES_RUN run, $$SUITES_FAILED failed"; \
	echo "============================================================"; \
	[ $$SUITES_FAILED -eq 0 ] && exit 0 || exit 1


# Convenience alias: `make asan`
asan: test_asan

clean_asan:
	rm -rf $(BUILD_DIR_ASAN) $(ASAN_BINS)

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR_ASAN) $(ALL_BINS) $(ASAN_BINS) *.o
	rm -rf $(BUILD_DIR_SAN) $(SAN_BINS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR_ASAN):
	mkdir -p $(BUILD_DIR_ASAN)

$(BUILD_DIR_SAN):
	mkdir -p $(BUILD_DIR_SAN)
