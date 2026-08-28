#ifndef HD60S_UTIL_H
#define HD60S_UTIL_H

#include <stdint.h>
#include <signal.h>

double now_s(void);
uint64_t now_mono_ns(void);
double now_monotonic_s(void);
int hex2bin(const char *hex, unsigned char *out, int maxlen);
void request_stop(int signal_number);
void install_signal_handlers(void);
int hd60s_env_present(const char *name);
int hd60s_env_on(const char *name);

extern volatile sig_atomic_t g_stop_requested;

#endif
