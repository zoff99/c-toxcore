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

.PHONY: all run clean

all: fuzz_toxutil

# ── Regression replay ────────────────────────────────────────────
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

.PHONY: regress
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

$(BUILD)/toxcore.o: $(AMALG_C) | $(BUILD)
	@echo "=== Compiling amalgamation (fuzzer+ASan+UBSan) ==="
	$(CC) -c $(CFLAGS) $(SAN) -fPIC $< -o $@

fuzz_toxutil: fuzz_toxutil.c $(BUILD)/toxcore.o
	@echo "=== Linking fuzz_toxutil ==="
	$(CC) $(CFLAGS) $(SAN) $< $(BUILD)/toxcore.o -o $@ $(LDLIBS)

run: fuzz_toxutil
	mkdir -p corpus
	./fuzz_toxutil $(FUZZ_ARGS) corpus/

clean:
	rm -rf $(BUILD) fuzz_toxutil replay corpus crash-* timeout-* oom-*

$(BUILD):
	mkdir -p $(BUILD)

