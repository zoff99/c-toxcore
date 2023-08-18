

#define _GNU_SOURCE


#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <unistd.h>
#include <getopt.h>
#include <fcntl.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <pthread.h>

#include <semaphore.h>
#include <signal.h>
#include <linux/sched.h>

#include <sodium.h>

#include <tox/tox.h>
#include <tox/toxav.h>

#define CURRENT_LOG_LEVEL 9 // 0 -> error, 1 -> warn, 2 -> info, 9 -> debug
const char *log_filename = "output.log";
const char *savedata_filename1 = "savedata1.tox";
const char *savedata_filename2 = "savedata2.tox";
FILE *logfile = NULL;

uint8_t s_num1 = 1;
uint8_t s_num2 = 2;

// #define USE_TOR 1
#define PROXY_PORT_TOR_DEFAULT 9050

int s_online[3] = { 0, 0, 0};
int f_online[3] = { 0, 0, 0};
int f_join_other[3] = { 0, 0, 0};
int f_join_self[3] = { 0, 0, 0};

int toxav_video_thread_stop = 0;
int toxav_audioiterate_thread_stop = 0;

long a_frames_rvcd = 0;
long v_frames_rvcd = 0;
long a_frames_sent = 0;
long v_frames_sent = 0;
uint64_t last_vframe_recv_ts = 0;
uint64_t last_vframe_recv_pts = 0;
uint64_t last_aframe_recv_ts = 0;
uint64_t last_aframe_recv_pts = 0;


// gives a counter value that increaes every millisecond
static uint64_t current_time_monotonic_default2()
{
    uint64_t time = 0;
    struct timespec clock_mono;
    clock_gettime(CLOCK_MONOTONIC, &clock_mono);
    time = 1000ULL * clock_mono.tv_sec + (clock_mono.tv_nsec / 1000000ULL);
    return time;
}

void dbg(int level, const char *fmt, ...)
{
    char *level_and_format = NULL;
    char *fmt_copy = NULL;

    if (fmt == NULL)
    {
        return;
    }

    if (strlen(fmt) < 1)
    {
        return;
    }

    if (!logfile)
    {
        return;
    }

    if ((level < 0) || (level > 9))
    {
        level = 0;
    }

    level_and_format = calloc(1, strlen(fmt) + 3 + 1);

    if (!level_and_format)
    {
        // dbg(9, stderr, "free:000a\n");
        return;
    }

    fmt_copy = level_and_format + 2;
    strcpy(fmt_copy, fmt);
    level_and_format[1] = ':';

    if (level == 0)
    {
        level_and_format[0] = 'E';
    }
    else if (level == 1)
    {
        level_and_format[0] = 'W';
    }
    else if (level == 2)
    {
        level_and_format[0] = 'I';
    }
    else
    {
        level_and_format[0] = 'D';
    }

    level_and_format[(strlen(fmt) + 2)] = '\0'; // '\0' or '\n'
    level_and_format[(strlen(fmt) + 3)] = '\0';
    struct timeval tv;
    gettimeofday(&tv, NULL);
    time_t t3 = time(NULL);
    struct tm tm3 = *localtime(&t3);
    char *level_and_format_2 = calloc(1, strlen(level_and_format) + 5 + 3 + 3 + 1 + 3 + 3 + 3 + 7 + 1);
    level_and_format_2[0] = '\0';
    snprintf(level_and_format_2, (strlen(level_and_format) + 5 + 3 + 3 + 1 + 3 + 3 + 3 + 7 + 1),
             "%04d-%02d-%02d %02d:%02d:%02d.%06ld:%s",
             tm3.tm_year + 1900, tm3.tm_mon + 1, tm3.tm_mday,
             tm3.tm_hour, tm3.tm_min, tm3.tm_sec, tv.tv_usec, level_and_format);

    if (level <= CURRENT_LOG_LEVEL)
    {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(logfile, level_and_format_2, ap);
        va_end(ap);
    }

    // dbg(9, "free:001\n");
    if (level_and_format)
    {
        // dbg(9, "free:001.a\n");
        free(level_and_format);
    }

    if (level_and_format_2)
    {
        free(level_and_format_2);
    }

    // dbg(9, "free:002\n");
}

uint32_t s_r(const uint32_t upper_bound)
{
    return randombytes_uniform(upper_bound);
}

uint32_t n_r(const uint32_t upper_bound)
{
    return rand() % upper_bound;
}

uint32_t generate_random_uint32()
{
#if 0
    // use libsodium function
    uint32_t r = randombytes_random();
    // dbg(9, "[%d]:random_sodium=%u\n", 0, r);
    return r;
#else
    // HINT: this is not perfectly randon, FIX ME!
    uint32_t r;
    r = rand();
    // dbg(9, "[%d]:random_glibc=%u\n", 0, r);
    return r;
#endif
}

void rvbuf(uint8_t *buf, size_t size)
{
    for (int i=0; i < size; i++)
    {
        // random value 1..255
        *buf = (uint8_t)((n_r(255)) + 1);
        buf++;
    }
}

int main(void)
{
    logfile = stdout;
    // logfile = fopen(log_filename, "wb");
    setvbuf(logfile, NULL, _IOLBF, 0);

    dbg(9, "--start--\n");

    // --- seed rand ---
    struct timeval time;
    gettimeofday(&time, NULL);
    srand((time.tv_sec * 1000) + (time.tv_usec / 1000));
    // --- seed rand ---

    bool res_enc;
    bool res_dec;
    int vbitrate = 101;
    int max_quantizer = 50;
    int w = 480; // 240;
    int h = 640; // 320;
    int y_bytes = w * h;
    int u_bytes = (w * h) / 4;
    int v_bytes = (w * h) / 4;
    uint8_t *y_buf = calloc(1, y_bytes);
    uint8_t *u_buf = calloc(1, u_bytes);
    uint8_t *v_buf = calloc(1, v_bytes);
    int w2 = 480 + 32; // 240 + 16; // with stride
    int h2 = 640; // 320;
    int y_bytes2 = w2 * h2;
    int u_bytes2 = (w2 * h2) / 4;
    int v_bytes2 = (w2 * h2) / 4;
    uint8_t *y_dec = calloc(1, y_bytes2 * 2);
    uint8_t *u_dec = calloc(1, u_bytes2 * 2);
    uint8_t *v_dec = calloc(1, v_bytes2 * 2);
    int32_t y_stride;
    int32_t u_stride;
    int32_t v_stride;
    uint8_t flush_decoder = 0;
    uint8_t *encoded_vframe = calloc(1, 50000);
    uint32_t encoded_frame_size_bytes = 0;
    void* tox_av_ngc_coders_global = toxav_ngc_video_init(vbitrate, max_quantizer);

    for(int f=0;f<200;f++)
    {
        if (f == 0)
        {
            rvbuf(y_buf, y_bytes);
            //rvbuf(u_buf, u_bytes);
            //rvbuf(v_buf, v_bytes);
        }

        if (f == 100)
        {
            vbitrate = 1500;
            max_quantizer = 21;
        }

        // dbg(9, "[%d]:false=%d true=%d\n", 0, (int)false, (int)true);
        res_enc = toxav_ngc_video_encode(tox_av_ngc_coders_global,
                                          vbitrate, max_quantizer,
                                          w, h,
                                          y_buf, u_buf, v_buf,
                                          encoded_vframe, &encoded_frame_size_bytes);
        dbg(9, "[%d]:frame:%d encoded vbitrate=%d size=%d raw size=%d res=%d\n", 0, f, vbitrate,
                        encoded_frame_size_bytes,
                        y_bytes + u_bytes + v_bytes,
                        res_enc);


        if (((f % 60) == 0) && (f != 0))
        {
            flush_decoder = 1;
        }
        else
        {
            flush_decoder = 0;
        }

        res_dec = toxav_ngc_video_decode(tox_av_ngc_coders_global,
                                            encoded_vframe,
                                            encoded_frame_size_bytes,
                                            w2, h2,
                                            y_dec, u_dec, v_dec,
                                            &y_stride, &u_stride, &v_stride, flush_decoder);
        dbg(9, "[%d]:frame:%d decoded size=%d %d %d (%d) res=%d\n", 0, f, (y_stride * h2), (u_stride * h2), (v_stride * h2),
                        ((y_stride * h2) + (u_stride * h2) + (v_stride * h2)), res_dec);
        dbg(9, "[%d]:frame:%d decoded ystride=%d ustride=%d vstride=%d\n", 0, f, y_stride, u_stride, v_stride);
    }

    toxav_ngc_video_kill(tox_av_ngc_coders_global);

    free(y_buf);
    free(u_buf);
    free(v_buf);
    free(y_dec);
    free(u_dec);
    free(v_dec);
    free(encoded_vframe);

    dbg(9, "--END--\n");
    fclose(logfile);

    return 0;
} 
