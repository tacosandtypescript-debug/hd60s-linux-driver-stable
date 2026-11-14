#ifndef HD60S_UTIL_H
#define HD60S_UTIL_H

#include <stdint.h>
#include <signal.h>

/* Rutas absolutas instaladas por make install; el servicio corre con WorkingDirectory en LIBEXEC. */
#define HD60S_LIBEXEC_DIR "/usr/local/libexec/hd60s"
#define HD60S_CAPTURE_DIR HD60S_LIBEXEC_DIR "/captures"

double now_s(void);
uint64_t now_mono_ns(void);
double now_monotonic_s(void);
int hex2bin(const char *hex, unsigned char *out, int maxlen);
void request_stop(int signal_number);
void install_signal_handlers(void);
int hd60s_env_present(const char *name);
int hd60s_env_on(const char *name);
/* Crea un directorio nuevo y privado (0700) bajo /run/hd60s. Devuelve 0 o -1. */
int hd60s_private_tmpdir(char *out, unsigned long out_n);

extern volatile sig_atomic_t g_stop_requested;

#endif
