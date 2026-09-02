#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>     /* fork, pipe, dup2, read, close, _exit */
#include <sys/wait.h>   /* waitpid, WIFEXITED, WIFSIGNALED ... */
#include <signal.h>     /* SIGABRT, SIGSEGV */
#include <errno.h>

/* ── ANSI colours ─────────────────────────────────────────────── */
#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_RED   "\033[31m"
#define C_GREEN "\033[32m"
#define C_CYAN  "\033[36m"

/* ── Counters (parent process only) ───────────────────────────── */
static int _tests_run    = 0;
static int _tests_passed = 0;
static int _tests_failed = 0;

/* ── Assertion macros (run inside the child) ──────────────────── */
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

#define T_ASSERT_INT_GE(a, b, msg) \
    do { long long _a=(long long)(a), _b=(long long)(b); if (_a<_b) { \
        printf(C_RED "      assertion failed: %s\n" C_RESET, (msg)); \
        printf("        at %s:%d: got %lld, want >= %lld\n", __func__, __LINE__, _a, _b); \
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

#define T_ASSERT_INT_NE(a, b, msg) \
    do { \
        long long _va = (long long)(a), _vb = (long long)(b); \
        if (_va == _vb) { \
            printf(C_RED "    FAIL  %s:%d  %s  (both %lld)\n" C_RESET, \
                   __func__, __LINE__, (msg), _va); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_INT_LE(a, b, msg) \
    do { \
        long long _va = (long long)(a), _vb = (long long)(b); \
        if (_va > _vb) { \
            printf(C_RED "    FAIL  %s:%d  %s  (got %lld, want <= %lld)\n" C_RESET, \
                   __func__, __LINE__, (msg), _va, _vb); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_INT_LT(a, b, msg) \
    do { \
        long long _va = (long long)(a), _vb = (long long)(b); \
        if (_va >= _vb) { \
            printf(C_RED "    FAIL  %s:%d  %s  (got %lld, want < %lld)\n" C_RESET, \
                   __func__, __LINE__, (msg), _va, _vb); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_STR_EQ(a, b, msg) \
    do { \
        const char *_sa = (a), *_sb = (b); \
        if (_sa == NULL || _sb == NULL || strcmp(_sa, _sb) != 0) { \
            printf(C_RED "    FAIL  %s:%d  %s  (got \"%s\", want \"%s\")\n" C_RESET, \
                   __func__, __LINE__, (msg), \
                   _sa ? _sa : "(null)", _sb ? _sb : "(null)"); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_PTR_NULL(p, msg) \
    do { \
        if ((p) != NULL) { \
            printf(C_RED "    FAIL  %s:%d  %s  (not NULL)\n" C_RESET, \
                   __func__, __LINE__, (msg)); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_MEM_EQ(a, b, len, msg) \
    do { \
        if (memcmp((a), (b), (len)) != 0) { \
            printf(C_RED "    FAIL  %s:%d  %s  (memory differs)\n" C_RESET, \
                   __func__, __LINE__, (msg)); \
            return false; \
        } \
    } while (0)

#define T_ASSERT_PTR_NOT_NULL(p, msg) \
    do { if ((p)==NULL) { \
        printf(C_RED "      assertion failed: %s\n" C_RESET, (msg)); \
        printf("        at %s:%d: pointer is NULL\n", __func__, __LINE__); \
        return false; \
    } } while (0)

/* ── Helpers: capture child output via a pipe ─────────────────── */
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
        if (r == 0) break;               /* EOF: child exited */
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
        while (i < len && buf[i] != '\n') { fputc(buf[i], stdout); i++; }
        fputc('\n', stdout);
        if (i < len && buf[i] == '\n') i++;
    }
}

/* ── Run one test in an isolated child process ────────────────── */
typedef bool (*_tf_test_fn)(void);

static void _tf_run_isolated(const char *name, _tf_test_fn fn)
{
    _tests_run++;
    fflush(stdout); fflush(stderr);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        /* fallback: run in-process (no isolation) */
        printf("    %-50s", name);
        if (fn()) { _tests_passed++; printf(C_GREEN "PASS\n" C_RESET); }
        else      { _tests_failed++; printf(C_RED   "FAIL\n" C_RESET); }
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        printf("    %-50s" C_RED "FAIL (fork error)\n" C_RESET, name);
        _tests_failed++;
        close(pipefd[0]); close(pipefd[1]);
        return;
    }

    if (pid == 0) {
        /* CHILD: send stdout+stderr into the pipe, run the test */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        bool ok = fn();
        _exit(ok ? 0 : 1);
    }

    /* PARENT: drain child output, then reap its status */
    close(pipefd[1]);
    size_t out_len = 0;
    char *out = _tf_read_all(pipefd[0], &out_len);

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        _tests_passed++;
        printf("    %-50s" C_GREEN "PASS\n" C_RESET, name);
    } else {
        _tests_failed++;
        if (WIFSIGNALED(status)) {
            int sig = WTERMSIG(status);
            const char *why =
                (sig == SIGABRT) ? "SIGABRT — likely ASAN / abort" :
                (sig == SIGSEGV) ? "SIGSEGV — segmentation fault" : "crashed";
            printf("    %-50s" C_RED "FAIL (%s, signal %d)\n" C_RESET, name, why, sig);
        } else {
            printf("    %-50s" C_RED "FAIL (exit %d)\n" C_RESET, name, WEXITSTATUS(status));
        }
        if (out && out_len > 0) {
            _tf_print_indented(out, out_len);
        }
    }
    free(out);
}

/* ── Suite / runner macros ────────────────────────────────────── */
#define RUN_TEST(fn) _tf_run_isolated(#fn, fn)

#define TEST_SUITE(name) printf(C_CYAN "  [%s]\n" C_RESET, (name))
#define SUITE_END()      printf("\n")

/* ── Summary ──────────────────────────────────────────────────── */
static inline int test_summary(const char *label)
{
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
    return (_tests_failed > 0) ? 1 : 0;
}

#endif /* TEST_FRAMEWORK_H */
