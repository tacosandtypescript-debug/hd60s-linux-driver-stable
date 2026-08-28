#include "hd60s_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

volatile sig_atomic_t g_stop_requested = 0;

double now_s(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

uint64_t now_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

double now_monotonic_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int hex2bin(const char* hex, unsigned char* out, int maxlen) {
    int n = 0; const char* p = hex;
    while (p[0] && p[1] && n < maxlen) {
        int hi, lo;
        char c = p[0]; hi = (c<='9')?c-'0':(c|32)-'a'+10;
        c = p[1]; lo = (c<='9')?c-'0':(c|32)-'a'+10;
        out[n++] = (hi<<4)|lo; p += 2;
    }
    return n;
}

void request_stop(int signal_number) {
    (void)signal_number;
    g_stop_requested = 1;
}

void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = request_stop;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
}

#define HD60S_ENV_CACHE 32
static struct {
    const char *name;
    const char *val;
    int filled;
} g_env_cache[HD60S_ENV_CACHE];

static const char *hd60s_getenv_cached(const char *name) {
    int empty = -1;
    for (int i = 0; i < HD60S_ENV_CACHE; i++) {
        if (g_env_cache[i].filled && g_env_cache[i].name == name)
            return g_env_cache[i].val;
        if (!g_env_cache[i].filled && empty < 0)
            empty = i;
    }
    const char *val = getenv(name);
    if (empty >= 0) {
        g_env_cache[empty].name = name;
        g_env_cache[empty].val = val;
        g_env_cache[empty].filled = 1;
    }
    return val;
}

int hd60s_env_present(const char *name) {
    return hd60s_getenv_cached(name) != NULL;
}

int hd60s_env_on(const char *name) {
    const char *value = hd60s_getenv_cached(name);
    return value && value[0] && value[0] != '0';
}
