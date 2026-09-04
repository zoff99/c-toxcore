#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_RED   "\033[31m"
#define C_GREEN "\033[32m"
#define C_CYAN  "\033[36m"

static int _tests_run    = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;

static int  _suites_run      = 0;
static int  _suites_failed   = 0;
static int  _suite_fail_base = 0;
static bool _suite_active    = false;

static inline void _tf_close_active_suite(void)
{
    if (_suite_active) {
        if (_tests_failed > _suite_fail_base) {
            _suites_failed++;
        }
        _suite_active = false;
    }
}

#define T_ASSERT_TRUE(expr, msg) \
    do { if (!(expr)) { \
        printf(C_RED "      assertion failed: %s\n" C_RESET, (msg)); \
        printf("        at %s:%d: %s\n", __func__, __LINE__, #expr); \
        return false; \
    } } while (0)

#define T_ASSERT_FALSE(expr, msg) T_ASSERT_TRUE(!(expr), msg)

#define T_ASSERT_INT_EQ(a, b, msg) \
    do { long long _a=(long long)(a), _b=(long long)(b); if (_a!=_b) { \
        printf(C_RED "      assertion failed: %s\n" C_RESET, (msg)); \
        printf("        at %s:%d: got %lld, want %lld\n", __func__, __LINE__, _a, _b); \
        return false; \
    } } while (0)

#define T_ASSERT_INT_GT(a, b, msg) \
    do { \
        long long _va = (long long)(a), _vb = (long long)(b); \
        if (_va <= _vb) { \
            printf(C_RED "    FAIL  %s:%d  %s  (got %lld, want > %lld)\n" C_RESET, \
                   __func__, __LINE__, (msg), _va, _vb); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_PTR_NOT_NULL(p, msg) \
    do { if ((p)==NULL) { \
        printf(C_RED "      assertion failed: %s\n" C_RESET, (msg)); \
        printf("        at %s:%d: pointer is NULL\n", __func__, __LINE__); \
        return false; \
    } } while (0)

static char *_tf_read_all(int fd, size_t *out_len)
{
    size_t cap = 8192, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); *out_len = 0; return NULL; }

    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = (char *)realloc(buf, cap);
            if (!nb) break;
            buf = nb;
        }

        ssize_t r = read(fd, buf + len, 4096);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        len += (size_t)r;
    }

    close(fd);
    *out_len = len;
    return buf;
}

static void _tf_print_indented(const char *buf, size_t len)
{
    size_t i = 0;

    while (i < len) {
        fputs("        ", stdout);

        while (i < len && buf[i] != '\n') {
            fputc(buf[i], stdout);
            i++;
        }

        fputc('\n', stdout);

        if (i < len && buf[i] == '\n') {
            i++;
        }
    }
}

typedef bool (*_tf_test_fn)(void);

static void _tf_run_isolated(const char *name, _tf_test_fn fn)
{
    _tests_run++;

    fflush(stdout);
    fflush(stderr);

    int pipefd[2];

    if (pipe(pipefd) != 0) {
        printf("    %-50s", name);

        if (fn()) {
            _tests_passed++;
            printf(C_GREEN "PASS\n" C_RESET);
        } else {
            _tests_failed++;
            printf(C_RED "FAIL\n" C_RESET);
        }

        return;
    }

    pid_t pid = fork();

    if (pid == -1) {
        printf("    %-50s" C_RED "FAIL (fork error)\n" C_RESET, name);
        _tests_failed++;
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        close(pipefd[0]);

        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);

        close(pipefd[1]);

        bool ok = fn();
        _exit(ok ? 0 : 1);
    }

    close(pipefd[1]);

    size_t out_len = 0;
    char *out = _tf_read_all(pipefd[0], &out_len);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        _tests_passed++;
        printf("    %-50s" C_GREEN "PASS\n" C_RESET, name);

        /*
         * HINT: this prints the stdout of the test also in case of PASS
         * if (out && out_len > 0) {
         *     _tf_print_indented(out, out_len);
         * }
         */
    } else {
        _tests_failed++;

        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            const char *why =
                (sig == SIGABRT) ? "SIGABRT" :
                (sig == SIGSEGV) ? "SIGSEGV" :
                "crashed";

            printf("    %-50s" C_RED "FAIL (%s, signal %d)\n" C_RESET,
                   name, why, sig);
        } else {
            printf("    %-50s" C_RED "FAIL (exit %d)\n" C_RESET,
                   name, WEXITSTATUS(status));
        }

        if (out && out_len > 0) {
            _tf_print_indented(out, out_len);
        }
    }

    free(out);
}

#define RUN_TEST(fn) _tf_run_isolated(#fn, fn)

#define TEST_SUITE(name) \
    do { \
        _tf_close_active_suite(); \
        printf(C_CYAN "  [%s]\n" C_RESET, (name)); \
        _suites_run++; \
        _suite_fail_base = _tests_failed; \
        _suite_active = true; \
    } while (0)

#define SUITE_END() \
    do { \
        _tf_close_active_suite(); \
        printf("\n"); \
    } while (0)

static inline int test_summary(const char *label)
{
    _tf_close_active_suite();

    printf(C_BOLD "  ───────────────────────────────────────────\n" C_RESET);

    if (_tests_failed == 0) {
        printf(C_GREEN C_BOLD "  ✓  %s:  %d / %d passed\n" C_RESET,
               label, _tests_passed, _tests_run);
    } else {
        printf(C_RED C_BOLD "  ✗  %s:  %d / %d passed,  %d FAILED\n" C_RESET,
               label, _tests_passed, _tests_run, _tests_failed);
    }

    printf(C_BOLD "  ───────────────────────────────────────────\n" C_RESET);

    printf("  Total:   %d\n", _tests_run);
    printf("  Passed:  %d\n", _tests_passed);
    printf("  Failed:  %d\n", _tests_failed);

    const char *grand_file = getenv("TEST_GRAND_FILE");
    if (grand_file && grand_file[0] != '\0') {
        FILE *f = fopen(grand_file, "a");
        if (f) {
            fprintf(f, "%d %d %d %d %d\n",
                    _tests_run,
                    _tests_passed,
                    _tests_failed,
                    _suites_run,
                    _suites_failed);
            fclose(f);
        }
    }

    return (_tests_failed > 0 || _suites_failed > 0) ? 1 : 0;
}

#endif
