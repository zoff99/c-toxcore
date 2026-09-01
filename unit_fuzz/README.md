# unit_fuzz

Fuzzing for toxutil.c using libFuzzer (built into Clang — no extra libraries).

## Requirements
- clang
- libsodium-dev (pkg-config must find libsodium)

## Run
    make run

Fuzzes until you press Ctrl-C. Crashing inputs are saved as `crash-<hash>`.

## Reproduce a crash
    ./fuzz_toxutil crash-<hash>

## Clean
    make clean
