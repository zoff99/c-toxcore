/*
 * tox_friend_file_sawtooth_test.c
 *
 * Minimal test:
 *   - tox1 and tox2 start TCP-only.
 *   - tox1 friends tox2.
 *   - Wait until friend connection is established on both sides.
 *   - tox1 sends a virtual 50 GiB file.
 *   - Sender generates random bytes on demand.
 *   - Receiver discards bytes.
 *   - Live terminal graph shows current KiB/s.
 *
 * Compile example:
 *   gcc -g -O2 -o tox_friend_file_sawtooth_test \
 *       tox_friend_file_sawtooth_test.c \
 *       ../amalgamation/libtoxcore.a \
 *       -lsodium -lopus -lx265 -lx264 -lavcodec -lvpx -lavutil
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <inttypes.h>

#include "../toxcore/tox.h"

#define USE_SOCKS5_PROXY 1

#define VIRTUAL_FILE_SIZE_BYTES (50ULL * 1024ULL * 1024ULL * 1024ULL)

#define SPEED_HISTORY_WIDTH  100
#define SPEED_GRAPH_HEIGHT   20
#define SPEED_SAMPLE_SECONDS 0.50

static volatile sig_atomic_t running = 1;

static void signal_handler(int signum)
{
    (void)signum;
    running = 0;
}

/* ---------- Bootstrap nodes ---------- */

typedef struct {
    const char *host;
    uint16_t    port;
    const char *public_key_hex;
} Bootstrap_Node;

static const Bootstrap_Node BOOTSTRAP_NODES[] = {
    { "tox.novg.net",      33445, "D527E5847F8330D628DAB1814F0A422F6DC9D0A300E6C357634EE2DA88C35463" },
    { "tox.initramfs.io",  33445, "3F0A45A268367C1BEA652F258C85F4A66DA76BCAA667A49E770BCC4917AB6A25" },
    { "tox.kurnevsky.net", 33445, "82EF82BA33445A1F91A7DB27189ECFC0C013E8E8A64D829A508B21C0250B0139" },
    { "144.217.167.73",   33445, "7E5668E0EE09E19F320AD47902419331FFEE147BB3606769CFBE921A2A2FD34C" },
    { "tox.abilinski.com", 33445, "10C00EB250C3233E343E2AEBA07115A5C28920E9C8D29492F6D00B29049EDC7E" },
    { "tox1.mf-net.eu",   33445, "B3E5FA80DC8EBD1149AD2AB35ED8B85BD546DEDE261CA593234C619249419506" },
    { "3.0.24.15",        33445, "E20ABCF38CDBFFD7D04B29C956B33F7B27A3BB7AF0618101617B036E4AEA402D" },
};

#define BOOTSTRAP_COUNT (sizeof(BOOTSTRAP_NODES) / sizeof(BOOTSTRAP_NODES[0]))

/* ---------- Helpers ---------- */

static double now_monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }

    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }

    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }

    return -1;
}

static bool parse_hex_key(const char *hex, uint8_t *out)
{
    if (!hex || !out) {
        return false;
    }

    const size_t len = strlen(hex);

    if (len != TOX_PUBLIC_KEY_SIZE * 2) {
        return false;
    }

    for (size_t i = 0; i < TOX_PUBLIC_KEY_SIZE; ++i) {
        const int h = hex_digit(hex[i * 2]);
        const int l = hex_digit(hex[i * 2 + 1]);

        if (h < 0 || l < 0) {
            return false;
        }

        out[i] = (uint8_t)((h << 4) | l);
    }

    return true;
}

static void log_msg(const char *who, const char *fmt, ...)
{
    char buf[64];
    const time_t now = time(NULL);
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime(&now));

    printf("[%s][%s] ", buf, who);

    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    printf("\n");
    fflush(stdout);
}

static void bootstrap(Tox *tox)
{
    for (size_t i = 0; i < BOOTSTRAP_COUNT; ++i) {
        uint8_t pk[TOX_PUBLIC_KEY_SIZE];

        if (!parse_hex_key(BOOTSTRAP_NODES[i].public_key_hex, pk)) {
            continue;
        }

        Tox_Err_Bootstrap e1;
        Tox_Err_Bootstrap e2;

        tox_add_tcp_relay(tox, BOOTSTRAP_NODES[i].host, BOOTSTRAP_NODES[i].port, pk, &e1);
        tox_bootstrap(tox, BOOTSTRAP_NODES[i].host, BOOTSTRAP_NODES[i].port, pk, &e2);
    }
}

/* ---------- State ---------- */

typedef struct TestState TestState;

typedef struct {
    const char *name;
    TestState  *test;

    bool        net_connected;

    uint32_t    friend_number;
    bool        friend_connected;

    uint32_t    file_number;
} ClientState;

typedef struct {
    uint64_t file_size;
    uint64_t bytes_received;
    uint64_t bytes_sent_by_callback;

    bool offer_seen;
    bool transfer_started;
    bool transfer_finished;

    double transfer_start_time;
    double last_sample_time;

    uint64_t last_sample_bytes;
    double current_kib_s;
    double average_kib_s;

    double samples[SPEED_HISTORY_WIDTH];
    size_t sample_count;
    size_t sample_next;

    uint8_t *send_buf;
    size_t send_buf_cap;

    uint64_t rng_state;
} FileStats;

struct TestState {
    ClientState tox1_state;
    ClientState tox2_state;

    FileStats file;
};

/* ---------- Random byte generator ---------- */

static uint64_t xorshift64star(uint64_t *state)
{
    uint64_t x = *state;

    if (x == 0) {
        x = 0x123456789abcdefULL;
    }

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;

    *state = x;

    return x * 2685821657736338717ULL;
}

static void fill_random_bytes(FileStats *fs, uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        const uint64_t r = xorshift64star(&fs->rng_state);
        const size_t n = (len - off >= sizeof(r)) ? sizeof(r) : (len - off);
        memcpy(buf + off, &r, n);
        off += n;
    }
}

/* ---------- Logging callback ---------- */

static void tox_log_cb__custom(Tox *tox, TOX_LOG_LEVEL level,
                               const char *file, uint32_t line,
                               const char *func, const char *message,
                               void *ud)
{
    (void)tox;

    ClientState *cs = (ClientState *)ud;
/*
    fprintf(stderr,
            "[%s] C-TOXCORE:%d:%s:%u:%s:%s\n",
            cs ? cs->name : "?",
            (int)level,
            file ? file : "?",
            line,
            func ? func : "?",
            message ? message : "?");

    fflush(stderr);
*/
}

/* ---------- Tox creation ---------- */

static Tox *create_tox(ClientState *cs)
{
    Tox_Err_Options_New oerr;
    struct Tox_Options *opts = tox_options_new(&oerr);

    if (!opts) {
        fprintf(stderr, "tox_options_new failed: %d\n", (int)oerr);
        exit(1);
    }

    tox_options_set_ipv6_enabled(opts, false);
    tox_options_set_udp_enabled(opts, false);              /* TCP-only */
    tox_options_set_local_discovery_enabled(opts, false);
    tox_options_set_hole_punching_enabled(opts, false);

#if USE_SOCKS5_PROXY
    tox_options_set_proxy_type(opts, TOX_PROXY_TYPE_SOCKS5);
    tox_options_set_proxy_host(opts, "localhost");
    tox_options_set_proxy_port(opts, 9050);
#endif

    tox_options_set_log_user_data(opts, cs);
    tox_options_set_log_callback(opts, tox_log_cb__custom);

    Tox_Err_New err;
    Tox *tox = tox_new(opts, &err);

    tox_options_free(opts);

    if (!tox) {
        fprintf(stderr, "tox_new failed: %d\n", (int)err);
        exit(1);
    }

    return tox;
}

/* ---------- Friend callbacks ---------- */

static void cb_self_conn(Tox *tox, Tox_Connection status, void *ud)
{
    (void)tox;

    ClientState *cs = (ClientState *)ud;
    const bool conn = status != TOX_CONNECTION_NONE;

    if (cs->net_connected != conn) {
        log_msg(cs->name, "Network: %s", conn ? "CONNECTED" : "NONE");
    }

    cs->net_connected = conn;
}

static void cb_friend_request(Tox *tox,
                              const uint8_t *public_key,
                              const uint8_t *message,
                              size_t length,
                              void *ud)
{
    (void)message;
    (void)length;

    ClientState *cs = (ClientState *)ud;

    log_msg(cs->name, "Received friend request; accepting");

    Tox_Err_Friend_Add err;
    const uint32_t fn = tox_friend_add_norequest(tox, public_key, &err);

    if (err != TOX_ERR_FRIEND_ADD_OK) {
        log_msg(cs->name, "tox_friend_add_norequest failed: %d", (int)err);
        return;
    }

    cs->friend_number = fn;
    log_msg(cs->name, "Friend accepted; local friend number=%u", fn);
}

static void cb_friend_connection_status(Tox *tox,
                                        uint32_t friend_number,
                                        Tox_Connection connection_status,
                                        void *ud)
{
    (void)tox;

    ClientState *cs = (ClientState *)ud;

    if (cs->friend_number == UINT32_MAX) {
        cs->friend_number = friend_number;
    }

    if (friend_number != cs->friend_number) {
        return;
    }

    const bool connected = connection_status != TOX_CONNECTION_NONE;

    if (cs->friend_connected != connected) {
        log_msg(cs->name,
                "Friend %u connection: %s",
                friend_number,
                connected ? "CONNECTED" : "NONE");
    }

    cs->friend_connected = connected;
}

/* ---------- File callbacks ---------- */

static void cb_file_recv(Tox *tox,
                         uint32_t friend_number,
                         uint32_t file_number,
                         uint32_t kind,
                         uint64_t file_size,
                         const uint8_t *filename,
                         size_t filename_length,
                         void *ud)
{
    ClientState *cs = (ClientState *)ud;
    TestState *test = cs->test;
    FileStats *fs = &test->file;

    char namebuf[256];
    const size_t n = filename_length < sizeof(namebuf) - 1 ? filename_length : sizeof(namebuf) - 1;
    memcpy(namebuf, filename, n);
    namebuf[n] = '\0';

    log_msg(cs->name,
            "Incoming file: friend=%u file=%u kind=%u size=%" PRIu64 " name='%s'",
            friend_number,
            file_number,
            kind,
            file_size,
            namebuf);

    cs->file_number = file_number;

    fs->offer_seen = true;
    fs->file_size = file_size;
    fs->bytes_received = 0;
    fs->transfer_finished = false;

    Tox_Err_File_Control err;
    const bool ok = tox_file_control(tox,
                                     friend_number,
                                     file_number,
                                     TOX_FILE_CONTROL_RESUME,
                                     &err);

    if (!ok || err != TOX_ERR_FILE_CONTROL_OK) {
        log_msg(cs->name, "tox_file_control RESUME failed: ok=%d err=%d", (int)ok, (int)err);
        return;
    }

    fs->transfer_started = true;
    fs->transfer_start_time = now_monotonic_seconds();
    fs->last_sample_time = fs->transfer_start_time;
    fs->last_sample_bytes = 0;

    log_msg(cs->name, "Accepted file transfer");
}

static void cb_file_recv_chunk(Tox *tox,
                               uint32_t friend_number,
                               uint32_t file_number,
                               uint64_t position,
                               const uint8_t *data,
                               size_t length,
                               void *ud)
{
    (void)tox;
    (void)friend_number;
    (void)file_number;
    (void)position;
    (void)data;

    ClientState *cs = (ClientState *)ud;
    FileStats *fs = &cs->test->file;

    if (length == 0) {
        fs->transfer_finished = true;
        return;
    }

    /*
     * Intentionally discard received bytes.
     * We only count them for speed measurement.
     */
    fs->bytes_received += (uint64_t)length;

    if (fs->bytes_received >= fs->file_size) {
        fs->transfer_finished = true;
    }
}

static void cb_file_chunk_request(Tox *tox,
                                  uint32_t friend_number,
                                  uint32_t file_number,
                                  uint64_t position,
                                  size_t length,
                                  void *ud)
{
    ClientState *cs = (ClientState *)ud;
    TestState *test = cs->test;
    FileStats *fs = &test->file;

    if (length == 0) {
        return;
    }

    if (position >= fs->file_size) {
        return;
    }

    if (position + (uint64_t)length > fs->file_size) {
        length = (size_t)(fs->file_size - position);
    }

    if (length == 0) {
        return;
    }

    if (length > fs->send_buf_cap) {
        uint8_t *nbuf = (uint8_t *)realloc(fs->send_buf, length);

        if (!nbuf) {
            log_msg(cs->name, "Out of memory allocating send buffer of %zu bytes", length);
            running = 0;
            return;
        }

        fs->send_buf = nbuf;
        fs->send_buf_cap = length;
    }

    fill_random_bytes(fs, fs->send_buf, length);

    Tox_Err_File_Send_Chunk err;
    const bool ok = tox_file_send_chunk(tox,
                                        friend_number,
                                        file_number,
                                        position,
                                        fs->send_buf,
                                        length,
                                        &err);

    if (!ok || err != TOX_ERR_FILE_SEND_CHUNK_OK) {
        log_msg(cs->name,
                "tox_file_send_chunk failed: pos=%" PRIu64 " len=%zu ok=%d err=%d",
                position,
                length,
                (int)ok,
                (int)err);
        return;
    }

    fs->bytes_sent_by_callback += (uint64_t)length;
}

static void cb_file_recv_control(Tox *tox,
                                 uint32_t friend_number,
                                 uint32_t file_number,
                                 Tox_File_Control control,
                                 void *ud)
{
    (void)tox;

    ClientState *cs = (ClientState *)ud;

    log_msg(cs->name,
            "File control: friend=%u file=%u control=%d",
            friend_number,
            file_number,
            (int)control);
}

/* ---------- Callback registration ---------- */

static void register_callbacks(Tox *tox, ClientState *cs)
{
    tox_callback_self_connection_status(tox, cb_self_conn);
    tox_callback_friend_request(tox, cb_friend_request);
    tox_callback_friend_connection_status(tox, cb_friend_connection_status);

    tox_callback_file_recv(tox, cb_file_recv);
    tox_callback_file_recv_chunk(tox, cb_file_recv_chunk);
    tox_callback_file_chunk_request(tox, cb_file_chunk_request);
    tox_callback_file_recv_control(tox, cb_file_recv_control);
}

/* ---------- Speed chart ---------- */

static void speed_push_sample(FileStats *fs, double kib_s)
{
    fs->samples[fs->sample_next] = kib_s;
    fs->sample_next = (fs->sample_next + 1) % SPEED_HISTORY_WIDTH;

    if (fs->sample_count < SPEED_HISTORY_WIDTH) {
        fs->sample_count++;
    }

    fs->current_kib_s = kib_s;
}

static double speed_sample_at(const FileStats *fs, size_t logical_index)
{
    if (logical_index >= fs->sample_count) {
        return 0.0;
    }

    size_t start;

    if (fs->sample_count < SPEED_HISTORY_WIDTH) {
        start = 0;
    } else {
        start = fs->sample_next;
    }

    const size_t idx = (start + logical_index) % SPEED_HISTORY_WIDTH;
    return fs->samples[idx];
}

static void update_speed_sample(FileStats *fs)
{
    const double now = now_monotonic_seconds();
    const double dt = now - fs->last_sample_time;

    if (dt < SPEED_SAMPLE_SECONDS) {
        return;
    }

    const uint64_t cur = fs->bytes_received;
    const uint64_t delta = cur - fs->last_sample_bytes;

    const double kib_s = ((double)delta / 1024.0) / dt;

    fs->last_sample_time = now;
    fs->last_sample_bytes = cur;

    speed_push_sample(fs, kib_s);

    const double elapsed = now - fs->transfer_start_time;
    if (elapsed > 0.0) {
        fs->average_kib_s = ((double)cur / 1024.0) / elapsed;
    }
}

static void render_speed_chart(const FileStats *fs)
{
    double max_kib_s = 1.0;

    for (size_t i = 0; i < fs->sample_count; ++i) {
        const double v = speed_sample_at(fs, i);

        if (v > max_kib_s) {
            max_kib_s = v;
        }
    }

    max_kib_s *= 1.15;

    const double elapsed = fs->transfer_started
                         ? now_monotonic_seconds() - fs->transfer_start_time
                         : 0.0;

    const double mib_done = (double)fs->bytes_received / (1024.0 * 1024.0);
    const double gib_done = (double)fs->bytes_received / (1024.0 * 1024.0 * 1024.0);
    const double gib_total = (double)fs->file_size / (1024.0 * 1024.0 * 1024.0);
    const double pct = fs->file_size ? ((double)fs->bytes_received * 100.0 / (double)fs->file_size) : 0.0;

    printf("\033[H\033[J");       /* clear screen */
    printf("tox friend file-transfer speed test\n");
    printf("Virtual file: %.2f GiB\n", gib_total);
    printf("Received:     %.2f MiB / %.2f GiB  (%.4f%%)\n", mib_done, gib_done, pct);
    printf("Current:      %.2f KiB/s\n", fs->current_kib_s);
    printf("Average:      %.2f KiB/s\n", fs->average_kib_s);
    printf("Scale top:    %.2f KiB/s\n", max_kib_s);
    printf("Elapsed:      %.1f s\n", elapsed);
    printf("Finished:     %s\n", fs->transfer_finished ? "yes" : "no");
    printf("\n");

    for (int row = SPEED_GRAPH_HEIGHT; row >= 1; --row) {
        const double threshold = max_kib_s * ((double)row / (double)SPEED_GRAPH_HEIGHT);

        printf("%8.0f | ", threshold);

        for (size_t col = 0; col < SPEED_HISTORY_WIDTH; ++col) {
            double v = 0.0;

            if (col >= SPEED_HISTORY_WIDTH - fs->sample_count) {
                const size_t logical = col - (SPEED_HISTORY_WIDTH - fs->sample_count);
                v = speed_sample_at(fs, logical);
            }

            putchar(v >= threshold ? '#' : ' ');
        }

        putchar('\n');
    }

    printf("         +");
    for (size_t i = 0; i < SPEED_HISTORY_WIDTH + 2; ++i) {
        putchar('-');
    }
    printf("\n");
    printf("          oldest%*snewest\n", SPEED_HISTORY_WIDTH - 11, "");
    printf("\nPress Ctrl-C to stop.\n");

    fflush(stdout);
}

/* ---------- Friend setup ---------- */

static bool send_friend_request(Tox *tox1, ClientState *tox1_state, Tox *tox2)
{
    uint8_t tox2_address[TOX_ADDRESS_SIZE];
    tox_self_get_address(tox2, tox2_address);

    const uint8_t msg[] = "friend request from tox1";

    Tox_Err_Friend_Add err;
    const uint32_t fn = tox_friend_add(tox1,
                                       tox2_address,
                                       msg,
                                       sizeof(msg) - 1,
                                       &err);

    if (err != TOX_ERR_FRIEND_ADD_OK) {
        log_msg("tox1", "tox_friend_add failed: %d", (int)err);
        return false;
    }

    tox1_state->friend_number = fn;

    log_msg("tox1", "Sent friend request to tox2; local friend number=%u", fn);

    return true;
}

/* ---------- File send start ---------- */

static bool start_virtual_file_send(Tox *tox1, ClientState *tox1_state)
{
    const uint8_t filename[] = "virtual-random-50GiB.bin";

    Tox_Err_File_Send err;
    const uint32_t file_number = tox_file_send(tox1,
                                               tox1_state->friend_number,
                                               TOX_FILE_KIND_DATA,
                                               VIRTUAL_FILE_SIZE_BYTES,
                                               NULL,
                                               filename,
                                               sizeof(filename) - 1,
                                               &err);

    if (err != TOX_ERR_FILE_SEND_OK) {
        log_msg("tox1", "tox_file_send failed: %d", (int)err);
        return false;
    }

    tox1_state->file_number = file_number;

    log_msg("tox1",
            "Started virtual file send: file_number=%u size=%" PRIu64 " bytes",
            file_number,
            (uint64_t)VIRTUAL_FILE_SIZE_BYTES);

    return true;
}

/* ---------- Main ---------- */

int main(void)
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    TestState test;
    memset(&test, 0, sizeof(test));

    test.tox1_state.name = "tox1";
    test.tox1_state.test = &test;
    test.tox1_state.friend_number = UINT32_MAX;
    test.tox1_state.file_number = UINT32_MAX;

    test.tox2_state.name = "tox2";
    test.tox2_state.test = &test;
    test.tox2_state.friend_number = UINT32_MAX;
    test.tox2_state.file_number = UINT32_MAX;

    test.file.file_size = VIRTUAL_FILE_SIZE_BYTES;
    test.file.rng_state = 0xfeedfacecafebeefULL;

    Tox *tox1 = create_tox(&test.tox1_state);
    Tox *tox2 = create_tox(&test.tox2_state);

    register_callbacks(tox1, &test.tox1_state);
    register_callbacks(tox2, &test.tox2_state);

    log_msg("main", "Bootstrapping both clients TCP-only...");
    bootstrap(tox1);
    bootstrap(tox2);

    log_msg("main", "Waiting for network connectivity...");

    time_t wait_start = time(NULL);

    while (running && time(NULL) - wait_start < 120) {
        tox_iterate(tox1, &test.tox1_state);
        tox_iterate(tox2, &test.tox2_state);

        if (test.tox1_state.net_connected && test.tox2_state.net_connected) {
            break;
        }

        usleep(50000);
    }

    if (!test.tox1_state.net_connected || !test.tox2_state.net_connected) {
        log_msg("main", "Timeout waiting for network connectivity");
        goto cleanup_fail;
    }

    log_msg("main", "Both clients connected to network");

    if (!send_friend_request(tox1, &test.tox1_state, tox2)) {
        goto cleanup_fail;
    }

    log_msg("main", "Waiting until both sides have accepted friend relationship and are connected...");

    wait_start = time(NULL);
    time_t last_bs = 0;

    while (running && time(NULL) - wait_start < 180) {
        tox_iterate(tox1, &test.tox1_state);
        tox_iterate(tox2, &test.tox2_state);

        if (test.tox1_state.friend_connected &&
            test.tox2_state.friend_connected &&
            test.tox1_state.friend_number != UINT32_MAX &&
            test.tox2_state.friend_number != UINT32_MAX) {
            break;
        }

        const time_t now = time(NULL);

        if (now - last_bs >= 15) {
            bootstrap(tox1);
            bootstrap(tox2);
            last_bs = now;
        }

        usleep(20000);
    }

    if (!test.tox1_state.friend_connected || !test.tox2_state.friend_connected) {
        log_msg("main", "Timeout waiting for friend connection");
        goto cleanup_fail;
    }

    log_msg("main", "Both toxes are friends and connected");
    log_msg("main", "Starting 50 GiB virtual file transfer...");

    if (!start_virtual_file_send(tox1, &test.tox1_state)) {
        goto cleanup_fail;
    }

    /*
     * Hide cursor while drawing the realtime chart.
     */
    printf("\033[?25l");
    fflush(stdout);

    while (running && !test.file.transfer_finished) {
        tox_iterate(tox1, &test.tox1_state);
        tox_iterate(tox2, &test.tox2_state);

        if (test.file.transfer_started) {
            update_speed_sample(&test.file);
            render_speed_chart(&test.file);
        }

        /*
         * Small sleep keeps CPU sane while still allowing frequent iteration.
         * Increase if you want less CPU, decrease if you want maximum throughput.
         */
        usleep(2000);
    }

    update_speed_sample(&test.file);
    render_speed_chart(&test.file);

    printf("\033[?25h");
    fflush(stdout);

    if (test.file.transfer_finished) {
        log_msg("main", "Transfer finished");
    } else {
        log_msg("main", "Stopped before transfer finished");
    }

    free(test.file.send_buf);

    tox_kill(tox1);
    tox_kill(tox2);

    return test.file.transfer_finished ? 0 : 1;

cleanup_fail:
    printf("\033[?25h");
    fflush(stdout);

    free(test.file.send_buf);

    if (tox1) {
        tox_kill(tox1);
    }

    if (tox2) {
        tox_kill(tox2);
    }

    log_msg("main", "Done with failure");

    return 1;
}
