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

# ── Test binaries (normal — link against toxcore.o) ──────────────
TEST_SRCS  := $(wildcard test_*.c)
ALL_BINS   := $(TEST_SRCS:.c=)
ASAN_BINS  := $(addsuffix _asan,$(ALL_BINS))
SAN_BINS   := $(addsuffix _san,$(ALL_BINS))

# ── Static-exposed test binaries (compile amalgamation inline) ───
# Naming convention: static_test_*.c  (distinct from test_*.c so the
# normal build pattern doesn't try to link them against toxcore.o).
# The .c file must use:
#     #define static
#     #include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#     #undef static
# and NOT link against toxcore.o (it's compiled inline).
STATIC_TEST_SRCS := $(wildcard static_test_*.c)
STATIC_BINS      := $(STATIC_TEST_SRCS:.c=)
STATIC_ASAN_BINS := $(addsuffix _asan,$(STATIC_BINS))
STATIC_SAN_BINS  := $(addsuffix _san,$(STATIC_BINS))

# ── All bins for cleanup ─────────────────────────────────────────
ALL_ALL_BINS := $(ALL_BINS) $(ASAN_BINS) $(SAN_BINS) \
                $(STATIC_BINS) $(STATIC_ASAN_BINS) $(STATIC_SAN_BINS)

.PHONY: all test test_asan test_san test_all asan san \
        test_static test_static_asan test_static_san \
        clean clean_asan objs objs_asan objs_san

all: $(ALL_BINS)

# =====================================================================
# NORMAL BUILD (links against toxcore.o)
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
# ASAN BUILD (AddressSanitizer only — links against toxcore.o)
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
# SAN BUILD (ASan + UBSan combined — links against toxcore.o)
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
# STATIC-EXPOSED BUILDS (amalgamation compiled inline via #include)
# =====================================================================
# These tests do NOT link against toxcore.o. The test .c file must
# #include the amalgamation directly with:
#     #define static
#     #include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#     #undef static

static_test_%: static_test_%.c test_framework.h
	@echo "=== Building $@ (static exposed) ==="
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

static_test_%_asan: static_test_%.c test_framework.h
	@echo "=== Building $@ (static + ASAN) ==="
	$(CC) $(CFLAGS) $(ASAN_FLAGS) $< -o $@ $(LDLIBS)

static_test_%_san: static_test_%.c test_framework.h
	@echo "=== Building $@ (static + ASan+UBSan) ==="
	$(CC) $(CFLAGS) $(SAN_FLAGS) $< -o $@ $(LDLIBS)

test_static: $(STATIC_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING STATIC-EXPOSED TEST SUITES"
	@echo "============================================================"
	@if [ -z "$(STATIC_BINS)" ]; then \
		echo "  (no static_test_*.c files found)"; \
		exit 0; \
	fi
	@GT=0; GP=0; GF=0; SUITES_RUN=0; SUITES_FAILED=0; \
	for BIN in $(STATIC_BINS); do \
		echo ""; echo "---- $$BIN ----"; \
		OUT=$$(mktemp); \
		./$$BIN > "$$OUT" 2>&1; \
		RC=$$?; \
		cat "$$OUT"; \
		SUITES_RUN=$$((SUITES_RUN+1)); \
		if [ $$RC -ne 0 ]; then \
			SUITES_FAILED=$$((SUITES_FAILED+1)); \
			echo "    >>> STATIC SUITE '$$BIN' FAILED (exit $$RC) <<<"; \
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
	echo "  GRAND TOTAL (Static-exposed)"; \
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

test_static_asan: $(STATIC_ASAN_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING STATIC-EXPOSED SUITES UNDER AddressSanitizer"
	@echo "============================================================"
	@if [ -z "$(STATIC_ASAN_BINS)" ]; then \
		echo "  (no static_test_*.c files found)"; \
		exit 0; \
	fi
	@GT=0; GP=0; GF=0; SUITES_RUN=0; SUITES_FAILED=0; \
	for BIN in $(STATIC_ASAN_BINS); do \
		echo ""; echo "---- $$BIN ----"; \
		OUT=$$(mktemp); \
		ASAN_OPTIONS=abort_on_error=1:detect_leaks=1 \
			./$$BIN > "$$OUT" 2>&1; \
		RC=$$?; \
		cat "$$OUT"; \
		SUITES_RUN=$$((SUITES_RUN+1)); \
		if [ $$RC -ne 0 ]; then \
			SUITES_FAILED=$$((SUITES_FAILED+1)); \
			echo "    >>> ASAN STATIC SUITE '$$BIN' FAILED (exit $$RC) <<<"; \
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
	echo "  GRAND TOTAL (Static + ASAN)"; \
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

test_static_san: $(STATIC_SAN_BINS)
	@echo ""
	@echo "============================================================"
	@echo "  RUNNING STATIC-EXPOSED SUITES UNDER ASan + UBSan"
	@echo "============================================================"
	@if [ -z "$(STATIC_SAN_BINS)" ]; then \
		echo "  (no static_test_*.c files found)"; \
		exit 0; \
	fi
	@GT=0; GP=0; GF=0; SUITES_RUN=0; SUITES_FAILED=0; \
	for BIN in $(STATIC_SAN_BINS); do \
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
			echo "    >>> SAN STATIC SUITE '$$BIN' FAILED (exit $$RC) <<<"; \
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
	echo "  GRAND TOTAL (Static + ASan+UBSan)"; \
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

# =====================================================================
# COMBINED / CLEANUP
# =====================================================================

# Run normal + strictest sanitizer (normal tests) + static-exposed sanitizer
test_all: test test_san test_static_san

clean_asan:
	rm -rf $(BUILD_DIR_ASAN) $(ASAN_BINS) $(STATIC_ASAN_BINS)

clean:
	rm -rf $(BUILD_DIR) $(BUILD_DIR_ASAN) $(BUILD_DIR_SAN)
	rm -f $(ALL_ALL_BINS)
	rm -f *.o

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR_ASAN):
	mkdir -p $(BUILD_DIR_ASAN)

$(BUILD_DIR_SAN):
	mkdir -p $(BUILD_DIR_SAN)
