/*
 * replay_main.c — standalone runner for saved fuzz inputs (regressions).
 * Reads each file and feeds it to LLVMFuzzerTestOneInput under ASan+UBSan.
 * Exits non-zero if any input crashes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

extern int LLVMFuzzerInitialize(int *argc, char ***argv);
extern int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint8_t *read_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <input-file> [...]\n", argv[0]);
        return 2;
    }
    LLVMFuzzerInitialize(&argc, &argv);

    int failed = 0;
    for (int i = 1; i < argc; i++) {
        size_t size = 0;
        uint8_t *buf = read_file(argv[i], &size);
        if (!buf) { fprintf(stderr, "[replay] cannot read %s\n", argv[i]); failed = 1; continue; }
        fprintf(stderr, "[replay] %s (%zu bytes)\n", argv[i], size);
        LLVMFuzzerTestOneInput(buf, size);   /* if this crashes, ASan aborts */
        free(buf);
    }
    return failed;
}
