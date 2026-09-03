# unit_fuzz/Makefile
# Coverage-guided fuzzing of toxutil.c using libFuzzer (ships with Clang).
# Requires: clang, libsodium (pkg-config).

CC       := clang

AMALG_C  := ../amalgamation/toxcore_amalgamation_no_toxav.c
BUILD    := _build_fuzz

CFLAGS   := -std=gnu11 -D_GNU_SOURCE -DTOX_HAVE_TOXUTIL -g -O1 \
            -I. -I../amalgamation -I../toxcore -I../toxutil -Wno-unused-parameter
CFLAGS   += $(shell pkg-config --cflags libsodium)

# fuzzer + ASan + UBSan together for maximum coverage
SAN      := -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
            -fno-sanitize-recover=all

LDLIBS   := $(shell pkg-config --libs libsodium) -lpthread

# Auto-detect number of CPU cores (works on Linux and macOS)
NPROC ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# -jobs=1000000  -> run effectively forever (1 million individual fuzzing jobs)
# -workers=$(NPROC) -> spawn one worker process per CPU core
FUZZ_ARGS := -max_len=4096 -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 \
             -jobs=1000000 -workers=$(NPROC)

# ── Fuzz binaries ────────────────────────────────────────────────
# Normal fuzz targets (link against toxcore.o)
FUZZ_SRCS       := $(wildcard fuzz_*.c)
# Exclude static fuzz targets from the normal list
NORMAL_FUZZ_SRCS := $(filter-out fuzz_static_%.c,$(FUZZ_SRCS))
NORMAL_FUZZ_BINS := $(NORMAL_FUZZ_SRCS:.c=)

# Static-exposed fuzz targets (amalgamation compiled inline via #include)
# Naming convention: fuzz_static_*.c
STATIC_FUZZ_SRCS := $(wildcard fuzz_static_*.c)
STATIC_FUZZ_BINS := $(STATIC_FUZZ_SRCS:.c=)

ALL_FUZZ_BINS := $(NORMAL_FUZZ_BINS) $(STATIC_FUZZ_BINS)

.PHONY: all run run_static run_all clean regress

all: $(ALL_FUZZ_BINS)

# =====================================================================
# NORMAL FUZZ TARGETS (link against toxcore.o)
# =====================================================================
$(BUILD)/toxcore.o: $(AMALG_C) | $(BUILD)
	@echo "=== Compiling amalgamation (fuzzer+ASan+UBSan) ==="
	$(CC) -c $(CFLAGS) $(SAN) -fPIC $< -o $@

# Pattern rule: fuzz_%.c links against toxcore.o
# (but NOT fuzz_static_%.c — those are handled below)
fuzz_%: fuzz_%.c $(BUILD)/toxcore.o
	@echo "=== Linking $@ ==="
	$(CC) $(CFLAGS) $(SAN) $< $(BUILD)/toxcore.o -o $@ $(LDLIBS)

run: $(NORMAL_FUZZ_BINS)
	@echo "=== Running normal fuzz targets ==="
	mkdir -p corpus
	@for BIN in $(NORMAL_FUZZ_BINS); do \
		echo ""; echo "---- $$BIN ----"; \
		./$$BIN $(FUZZ_ARGS) corpus/; \
	done

# =====================================================================
# STATIC-EXPOSED FUZZ TARGETS (amalgamation compiled inline)
# =====================================================================
# These do NOT link against toxcore.o. The .c file must use:
#     #define static
#     #include "../amalgamation/toxcore_amalgamation_no_toxav.c"
#     #undef static

fuzz_static_%: fuzz_static_%.c
	@echo "=== Building $@ (static exposed) ==="
	$(CC) $(CFLAGS) $(SAN) $< -o $@ $(LDLIBS)

run_static: $(STATIC_FUZZ_BINS)
	@echo "=== Running static-exposed fuzz targets ==="
	mkdir -p corpus_static
	@for BIN in $(STATIC_FUZZ_BINS); do \
		echo ""; echo "---- $$BIN ----"; \
		./$$BIN $(FUZZ_ARGS) corpus_static/; \
	done

# Run both normal and static fuzz targets
run_all: run run_static

# =====================================================================
# REGRESSION REPLAY
# =====================================================================
SAN_REPLAY  := -fsanitize=address,undefined -fno-omit-frame-pointer \
               -fno-sanitize-recover=all
REGRESS_DIR := regressions

_build_fuzz/toxcore_replay.o: $(AMALG_C) | $(BUILD)
	$(CC) -c $(CFLAGS) $(SAN_REPLAY) -fPIC $< -o $@

_build_fuzz/fuzz_nofuzz.o: fuzz_toxutil.c | $(BUILD)
	$(CC) -c $(CFLAGS) $(SAN_REPLAY) $< -o $@

replay: replay_main.c _build_fuzz/fuzz_nofuzz.o _build_fuzz/toxcore_replay.o
	$(CC) $(CFLAGS) $(SAN_REPLAY) replay_main.c \
	    _build_fuzz/fuzz_nofuzz.o _build_fuzz/toxcore_replay.o -o $@ $(LDLIBS)

regress: replay
	@if [ ! -d $(REGRESS_DIR) ] || [ -z "$$(ls -A $(REGRESS_DIR) 2>/dev/null)" ]; then \
		echo "No regression files in $(REGRESS_DIR)/"; exit 0; \
	fi; \
	failed=0; \
	for f in $(REGRESS_DIR)/*; do \
		echo "=== replay $$f ==="; \
		out=$$(mktemp); \
		./replay "$$f" > "$$out" 2>&1; \
		rc=$$?; \
		if [ $$rc -ne 0 ]; then \
			echo "    >>> BUG PRESENT: $$f crashes (exit $$rc) <<<"; \
			sed -n '1,15p' "$$out"; \
			failed=1; \
		else \
			echo "    fixed: $$f no longer crashes"; \
		fi; \
		rm -f "$$out"; \
	done; \
	exit $$failed

# =====================================================================
# CLEANUP
# =====================================================================
clean:
	rm -rf $(BUILD) $(ALL_FUZZ_BINS) replay corpus corpus_static \
	       crash-* timeout-* oom-*

$(BUILD):
	mkdir -p $(BUILD)
