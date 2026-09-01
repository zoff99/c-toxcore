/*
 * fuzz_toxutil.c
 *
 * libFuzzer harness for toxutil.c (built from the amalgamation).
 *
 * It drives the attacker-controlled attack surface of toxutil.c:
 *   Target 0: tox_utils_file_recv_cb() + tox_utils_file_recv_chunk_cb()
 *             + tox_utils_file_recv_control_cb()  (the MessageV2 file-transfer
 *             path — this is where the heap-buffer-overflow lives).
 *   Target 1: tox_messagev2_get_*() parsing helpers (OOB-read surface).
 *
 * Build & run:  make run
 * Reproduce a crash:  ./fuzz_toxutil <crash-file>
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

#include <tox.h>
#include <toxutil.h>

/* Internal toxutil callbacks (non-static in the amalgamation). */
extern void tox_utils_file_recv_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint32_t kind, uint64_t file_size,
        const uint8_t *filename, size_t filename_length, void *user_data);
extern void tox_utils_file_recv_chunk_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, uint64_t position, const uint8_t *data,
        size_t length, void *user_data);
extern void tox_utils_file_recv_control_cb(Tox *tox, uint32_t friend_number,
        uint32_t file_number, TOX_FILE_CONTROL control, void *user_data);

static Tox     *g_tox    = NULL;
static uint32_t g_friend = 0;

/* One-time setup: create a minimal (network-disabled) Tox and one friend,
 * so the toxutil pubkey lookups succeed. Reused across all inputs for speed. */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;

    struct Tox_Options o;
    tox_options_default(&o);
    o.ipv6_enabled          = false;
    o.udp_enabled           = false;   /* no sockets needed for these funcs */
    o.local_discovery_enabled = false;
    o.hole_punching_enabled = false;
    o.tcp_port              = 0;

    g_tox = tox_utils_new(&o, NULL);
    if (!g_tox) return 1;

    uint8_t pk[TOX_PUBLIC_KEY_SIZE];
    memset(pk, 0x42, sizeof(pk));
    g_friend = tox_friend_add_norequest(g_tox, pk, NULL);
    return 0;
}

/* Safely consume up to `want` bytes from the input. */
static size_t take(const uint8_t *d, size_t n, size_t *pos, void *out, size_t want)
{
    size_t avail = (*pos < n) ? (n - *pos) : 0;
    size_t k = (avail < want) ? avail : want;
    if (k > 0) memcpy(out, d + *pos, k);
    *pos += k;
    return k;
}

/* ------------------------------------------------------------------
 * Target 0: MessageV2 file-transfer path
 * ------------------------------------------------------------------ */
static void fuzz_file_xfer(const uint8_t *d, size_t n)
{
    if (!g_tox) return;

    size_t pos = 0;

    uint8_t kb = 0;
    take(d, n, &pos, &kb, 1);
    static const uint32_t kinds[] = {
        TOX_FILE_KIND_MESSAGEV2_SEND,
        TOX_FILE_KIND_MESSAGEV2_SYNC,
        TOX_FILE_KIND_MESSAGEV2_ANSWER,
        TOX_FILE_KIND_DATA,
    };
    uint32_t kind = kinds[kb % 4];

    uint64_t file_size = 1024;
    take(d, n, &pos, &file_size, sizeof(file_size));

    const uint32_t file_number = 0;   /* fixed; cleaned up at end of each input */

    /* Register the incoming transfer (attacker controls kind + file_size). */
    tox_utils_file_recv_cb(g_tox, g_friend, file_number, kind, file_size,
                           (const uint8_t *)"f", 1, NULL);

    /* Deliver 1..3 chunks at attacker-controlled positions. */
    int rounds = 1 + (kb % 3);
    for (int r = 0; r < rounds && pos < n; r++) {
        uint64_t chunk_pos = 0;
        take(d, n, &pos, &chunk_pos, sizeof(chunk_pos));

        size_t avail = (pos < n) ? (n - pos) : 0;
        size_t chunk_len = (avail > 256) ? 256 : avail;   /* cap for speed */

        tox_utils_file_recv_chunk_cb(g_tox, g_friend, file_number,
                                     chunk_pos, d + pos, chunk_len, NULL);
        pos += chunk_len;
    }

    /* Clean up so global state stays bounded across inputs. */
    tox_utils_file_recv_chunk_cb(g_tox, g_friend, file_number, file_size, NULL, 0, NULL);
    tox_utils_file_recv_control_cb(g_tox, g_friend, file_number,
                                   TOX_FILE_CONTROL_CANCEL, NULL);
}

/* ------------------------------------------------------------------
 * Target 1: MessageV2 parsing helpers (OOB-read surface)
 * ------------------------------------------------------------------ */
static void fuzz_msgv2_getters(const uint8_t *d, size_t n)
{
    /* FIX: Allocate the full fixed-size buffer, just like toxutil.c does.
     * This stops ASan from complaining about OOB reads on tiny fuzz buffers,
     * and accurately tests how the code handles truncated/garbage messages
     * (it will just read zeros past the valid data). */
    uint8_t *buf = (uint8_t *)calloc(1, TOX_MAX_FILETRANSFER_SIZE_MSGV2);
    if (!buf) return;
    
    size_t copy_len = (n < TOX_MAX_FILETRANSFER_SIZE_MSGV2) ? n : TOX_MAX_FILETRANSFER_SIZE_MSGV2;
    memcpy(buf, d, copy_len);

    uint8_t id[TOX_PUBLIC_KEY_SIZE];
    uint8_t pk[TOX_PUBLIC_KEY_SIZE];

    /* These will now safely read zeros if the fuzz data is too short */
    tox_messagev2_get_message_id(buf, id);
    tox_messagev2_get_ts_sec(buf);
    tox_messagev2_get_ts_ms(buf);
    tox_messagev2_get_alter_type(buf);
    tox_messagev2_get_sync_message_type(buf);
    tox_messagev2_get_sync_message_pubkey(buf, pk);

    free(buf);
}

/* ------------------------------------------------------------------
 * libFuzzer entry point
 * ------------------------------------------------------------------ */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;

    /* Weight the file-transfer path higher — it's the critical attack surface. */
    if ((data[0] % 4) == 0) {
        fuzz_msgv2_getters(data + 1, size - 1);
    } else {
        fuzz_file_xfer(data + 1, size - 1);
    }
    return 0;
}
