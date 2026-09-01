SHELL      := /bin/bash
CC         ?= gcc

# ── Paths ─────────────────────────────────────────────────────────
AMALGAMATION_C := ../amalgamation/toxcore_amalgamation_no_toxav.c
BUILD_DIR      := _build
BUILD_DIR_ASAN := _build_asan
BUILD_DIR_SAN  := _build_san

# ── Compiler flags ───────────────────────────────────────────────
CFLAGS     := -std=gnu11 -D_GNU_SOURCE -DTOX_HAVE_TOXUTIL -Wall -Wextra -g -O0 \
              -I. -I../amalgamation -I../toxcore -I../toxutil -Wno-unused-parameter
CFLAGS     += $(shell pkg-config --cflags libsodium)

# ── Linker flags ─────────────────────────────────────────────────
LDLIBS     := $(shell pkg-config --libs libsodium) -lpthread

# ── Sanitizer flags ──────────────────────────────────────────────
ASAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer -O1 -g
SAN_FLAGS  := -fsanitize=address,undefined -fno-omit-frame-pointer -O1 -g \
              -fno-sanitize-recover=all

# ── Test binaries ────────────────────────────────────────────────
TEST_SRCS  := $(wildcard test_*.c)
ALL_BINS   := $(TEST_SRCS:.c=)
ASAN_BINS  := $(addsuffix _asan,$(ALL_BINS))
SAN_BINS   := $(addsuffix _san,$(ALL_BINS))

.PHONY: all test test_asan test_san test_all asan san clean clean_asan objs objs_asan objs_san

all: $(ALL_BINS)

# =====================================================================
# NORMAL BUILD
# =====================================================================
objs: $(BUILD_DIR)/toxcore.o

$(BUILD_DIR)/toxcore.o: $(AMALGAMATION_C) | $(BUILD_DIR)
	@echo "=== Compiling Toxcore Amalgamation (no ToxAV) ==="
	$(CC) -c $(CFLAGS) -O2 -fPIC -Wno-discarded-qualifiers $(AMALGAMATION_C) -o $@

test_%: test_%.c test_framework.h objs
	@echo "=== Building $@ ==="
	$(CC) $(CFLAGS) $< $(BUILD_DIR)/toxcore.o -o $@ $(LDLIBS)

test: $(ALL_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING ALL TEST SUITES (Normal)"
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
	echo "  GRAND TOTAL (Normal)"; \
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

# =====================================================================
# ASAN BUILD (AddressSanitizer only)
# =====================================================================
objs_asan: $(BUILD_DIR_ASAN)/toxcore.o

$(BUILD_DIR_ASAN)/toxcore.o: $(AMALGAMATION_C) | $(BUILD_DIR_ASAN)
	@echo "=== Compiling Toxcore Amalgamation with ASAN ==="
	$(CC) -c $(CFLAGS) $(ASAN_FLAGS) -fPIC -Wno-discarded-qualifiers $(AMALGAMATION_C) -o $@

test_%_asan: test_%.c test_framework.h objs_asan
	@echo "=== Building $@ (ASAN) ==="
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< $(BUILD_DIR_ASAN)/toxcore.o -o $@ $(LDLIBS)

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

asan: test_asan

# =====================================================================
# SAN BUILD (ASan + UBSan combined)
# =====================================================================
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
	@GT=0; GP=0; GF=0; SUITES_RUN=0; SUITES_FAILED=0; \
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
	echo "  GRAND TOTAL (ASan + UBSan)"; \
	echo "============================================================"; \
	echo "    Individual tests:  $$GT total, $$GP passed, $$GF failed"; \
	echo "    Suites:            $$SUITES_RUN run, $$SUITES_FAILED failed"; \
	echo "============================================================"; \
	if [ $$SUITES_FAILED -ne 0 ]; then \
		echo "  RESULT: FAIL (memory/UB errors detected)"; \
		exit 1; \
	else \
		echo "  RESULT: PASS (no memory/UB errors)"; \
		exit 0; \
	fi

san: test_san

# =====================================================================
# COMBINED / CLEANUP
# =====================================================================

# Run both normal and the strictest sanitizer suite
test_all: test test_san

clean_asan:
	rm -rf $(BUILD_DIR_ASAN) $(ASAN_BINS)

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR_ASAN) $(BUILD_DIR_SAN)
	rm -f $(ALL_BINS) $(ASAN_BINS) $(SAN_BINS)
	rm -f *.o

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR_ASAN):
	mkdir -p $(BUILD_DIR_ASAN)

$(BUILD_DIR_SAN):
	mkdir -p $(BUILD_DIR_SAN)
