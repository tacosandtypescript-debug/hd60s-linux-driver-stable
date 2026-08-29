#include "hd60s_audio.h"
#include "hd60s_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <samplerate.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>

static int g_use_pw = 0;

// ======================================================================
// SECTION 2: salida de audio ALSA (vía snd-aloop)
// ======================================================================
// salida ALSA snd-aloop
// estructura medida del SEP payload: 8B = 2 stereo S16_LE frames, interleaved as
// L0,R0,L1,R1. The observed SEP cadence is about 24,000 records/s, so each
// record contains two audio frames at the normal 48kHz HDMI rate. Downmix
// both frames to mono before resampling to the OBS bridge rate.
static snd_pcm_t* g_pcm = NULL;
static _Atomic unsigned long long g_audio_frames = 0;
static _Atomic unsigned long long g_audio_underrun = 0;
static _Atomic unsigned long long g_audio_packets = 0;
static _Atomic unsigned long long g_audio_bytes = 0;
static _Atomic unsigned long long g_audio_samples_in = 0;
static _Atomic unsigned long long g_audio_samples_out = 0;
static _Atomic unsigned long long g_audio_pw_underflow = 0;
#define AUDIO_TARGET_RATE 48000
#define AUDIO_BATCH_FRAMES 480  // 10 ms @ 48 kHz mono
static int16_t g_audio_buf[AUDIO_BATCH_FRAMES];  // mono
static SRC_STATE *g_src = NULL;
static int g_src_ok = 0;
static int g_audio_buf_pos = 0;

// ALSA playback must not run in the libusb callback.  snd_pcm_writei() can
// wait for the hardware clock, and doing that in the USB event thread stalls
// both parser_feed() and the resubmission path.  Keep a bounded SPSC queue of
// short batches; the producer only copies and signals, while the writer owns
// all blocking ALSA calls.
#define AUDIO_QUEUE_BATCHES 64
static int16_t g_audio_queue[AUDIO_QUEUE_BATCHES][AUDIO_BATCH_FRAMES];
static uint16_t g_audio_queue_len[AUDIO_QUEUE_BATCHES];
static _Atomic uint32_t g_audio_queue_head = 0;
static _Atomic uint32_t g_audio_queue_tail = 0;
static _Atomic unsigned long long g_audio_queue_drops = 0;
static _Atomic int g_audio_writer_stop = 0;
static pthread_t g_audio_writer_thread;
static int g_audio_writer_started = 0;
static pthread_mutex_t g_audio_writer_wake_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_audio_writer_wake_cond = PTHREAD_COND_INITIALIZER;

typedef struct {
    double measure_start_s;
    int measure_started;
    int measure_done;
    int sep_count;
    int active_sep_count;
    double last_recal_check_s;
    int rolling_sep_count;
    double measured_sep_rate;
} AudioCadenceState;

static AudioCadenceState g_audio_cadence = {0};
static int g_sep_count = 0;
// ratio hacia el puente 48 kHz (0 = aún no medido)
static double g_upsample_ratio = 0.0;
static double g_pll_base_ratio = 0.0;
static double g_pll_last_update = 0.0;
static double g_pll_integral = 0.0;
static unsigned long long g_pll_updates = 0;
// estado de interpolación: último sample de fin (origen de la siguiente interp)
static int16_t g_last_sample = 0;
// acumulado de delay fraccionario (gestión del resto de samples a emitir)
static double g_frac_pos = 0.0;

static void audio_track_cadence_and_recalibrate(int is_active) {
    double now = now_monotonic_s();
    g_sep_count++;
    g_audio_cadence.sep_count++;
    g_audio_cadence.rolling_sep_count++;
    if (is_active) g_audio_cadence.active_sep_count++;

    // 1. Initial measurement (ignoring early silence window)
    if (!g_audio_cadence.measure_done) {
        if (!g_audio_cadence.measure_started) {
            // Do not start measurement window during initial silence/settling
            if (!is_active || g_audio_cadence.active_sep_count < 100) {
                if (g_upsample_ratio <= 0.0) {
                    g_upsample_ratio = 1.0;
                    g_pll_base_ratio = g_upsample_ratio;
                }
                return;
            }
            g_audio_cadence.measure_start_s = now;
            g_audio_cadence.measure_started = 1;
            g_audio_cadence.sep_count = 0;
            g_audio_cadence.active_sep_count = 0;
            if (g_upsample_ratio <= 0.0) {
                g_upsample_ratio = 1.0;
                g_pll_base_ratio = g_upsample_ratio;
            }
            return;
        }

        double el = now - g_audio_cadence.measure_start_s;
        if (el >= 2.0 && g_audio_cadence.sep_count >= 500) {
            double sep_rate = (double)g_audio_cadence.sep_count / el;
            double audio_rate = sep_rate * 2.0;

            // Guard against partial silence / ~12.2k SEP/s artifact:
            // If sep_rate is < 18000 and the stream is still in its startup phase,
            // reset window to wait for a clean steady-state measurement window
            if (sep_rate < 18000.0 && el < 6.0) {
                g_audio_cadence.measure_start_s = now;
                g_audio_cadence.sep_count = 0;
                g_audio_cadence.active_sep_count = 0;
                return;
            }

            double effective_sample_rate = audio_rate;
            if (audio_rate >= 40000.0 && audio_rate <= 60000.0) {
                effective_sample_rate = audio_rate;
            } else if (audio_rate >= 88000.0 && audio_rate <= 105000.0) {
                effective_sample_rate = audio_rate;
            } else if (audio_rate >= 176000.0 && audio_rate <= 200000.0) {
                effective_sample_rate = audio_rate;
            } else {
                effective_sample_rate = 48000.0;
            }

            double target_rate = (double)AUDIO_TARGET_RATE;
            g_upsample_ratio = target_rate / effective_sample_rate;
            if (g_upsample_ratio < 0.125) g_upsample_ratio = 0.125;
            if (g_upsample_ratio > 8.0) g_upsample_ratio = 8.0;
            /* Solo identidad si el error es < ~24 muestras/s. Un snap a 1.0
             * con 47870 Hz reales vacía el anillo PipeWire y el audio se corta. */
            if (fabs(g_upsample_ratio - 1.0) < 0.0005)
                g_upsample_ratio = 1.0;
            g_pll_base_ratio = g_upsample_ratio;
            g_pll_last_update = now;
            g_audio_cadence.measured_sep_rate = sep_rate;
            g_audio_cadence.measure_done = 1;
            g_audio_cadence.last_recal_check_s = now;
            g_audio_cadence.rolling_sep_count = 0;
            g_last_sample = 0;
            g_frac_pos = 0.0;

            fprintf(stderr, "[audio%s] measured: %.1f SEP/s (%.1f audio frames/s) → %.0f Hz nominal → ratio %.3fx to %.0fkHz %s\n",
                    g_use_pw ? "-pw" : "", sep_rate, audio_rate, effective_sample_rate,
                    g_upsample_ratio, target_rate / 1000.0, g_use_pw ? "stream" : "alsa bridge");
        }
    } else {
        // 2. Continuous Monitoring & Automatic Recalibration
        double dt = now - g_audio_cadence.last_recal_check_s;
        if (dt >= 1.0) {
            double rolling_sep_rate = (double)g_audio_cadence.rolling_sep_count / dt;
            g_audio_cadence.rolling_sep_count = 0;
            g_audio_cadence.last_recal_check_s = now;

            if (rolling_sep_rate >= 18000.0) {
                double rolling_audio_rate = rolling_sep_rate * 2.0;
                double nominal_rate = 48000.0;
                if (rolling_audio_rate >= 40000.0 && rolling_audio_rate <= 60000.0) {
                    nominal_rate = 48000.0;
                } else if (rolling_audio_rate >= 88000.0 && rolling_audio_rate <= 105000.0) {
                    nominal_rate = 96000.0;
                } else if (rolling_audio_rate >= 176000.0 && rolling_audio_rate <= 200000.0) {
                    nominal_rate = 192000.0;
                }
                double target_rate = (double)AUDIO_TARGET_RATE;
                double expected_ratio = target_rate / nominal_rate;
                if (fabs(g_upsample_ratio - expected_ratio) > 0.02) {
                    fprintf(stderr, "[audio%s-recalibrate] ratio adjusted from %.4f to expected %.4f (measured %.1f SEP/s, nominal %.0f Hz)\n",
                            g_use_pw ? "-pw" : "", g_upsample_ratio, expected_ratio,
                            rolling_sep_rate, nominal_rate);
                    g_upsample_ratio = expected_ratio;
                    g_pll_base_ratio = expected_ratio;
                }
            }
        }
    }
}

// SEP payload: two signed 16-bit stereo frames, little-endian, L0,R0,L1,R1.
// Decode the four samples without relying on alignment, then downmix each
// stereo frame independently so adjacent channels are never combined as one
// 24-bit sample.
static int16_t decode_sep_s16(const uint8_t* p) {
    uint16_t raw;
    memcpy(&raw, p, sizeof(raw));
    return (int16_t)raw;
}

static void decode_sep_stereo(const uint8_t *payload, int16_t lr[4]) {
    lr[0] = decode_sep_s16(payload);
    lr[1] = decode_sep_s16(payload + 2);
    lr[2] = decode_sep_s16(payload + 4);
    lr[3] = decode_sep_s16(payload + 6);
}

static void decode_sep_mono(const uint8_t* payload, int16_t mono[2]) {
    int16_t lr[4];
    decode_sep_stereo(payload, lr);
    mono[0] = (int16_t)(((int32_t)lr[0] + lr[1]) / 2);
    mono[1] = (int16_t)(((int32_t)lr[2] + lr[3]) / 2);
}

static int audio_ratio_is_unity(void) {
    return fabs(g_upsample_ratio - 1.0) < 0.0005;
}

static void audio_src_init(void) {
    if (g_src_ok) return;
    int src_err = 0;
    g_src = src_new(SRC_SINC_MEDIUM_QUALITY, 2, &src_err);
    if (!g_src) {
        fprintf(stderr, "[audio] src_new failed: %s; using linear fallback\n",
                src_strerror(src_err));
        g_src_ok = 0;
        return;
    }
    g_src_ok = 1;
    fprintf(stderr, "[audio] libsamplerate SINC_MEDIUM_QUALITY enabled\n");
}

static void audio_src_close(void) {
    if (g_src) {
        src_delete(g_src);
        g_src = NULL;
    }
    g_src_ok = 0;
}

// 2 frames estéreo HDMI → interleaved S16. A ratio 1.0 no se filtra (el SINC ensucia).
static int audio_resample_sep_stereo(const int16_t lr[4], int16_t *out, int max_samples) {
    if (max_samples < 4) return 0;
    if (audio_ratio_is_unity()) {
        memcpy(out, lr, 4 * sizeof(int16_t));
        return 4;
    }
    if (g_src_ok) {
        float input[4] = {
            (float)lr[0] / 32768.0f, (float)lr[1] / 32768.0f,
            (float)lr[2] / 32768.0f, (float)lr[3] / 32768.0f,
        };
        float output[64];
        int cap_frames = (int)(sizeof(output) / sizeof(output[0]) / 2);
        if (cap_frames > max_samples / 2) cap_frames = max_samples / 2;
        SRC_DATA data = {
            .data_in = input,
            .input_frames = 2,
            .data_out = output,
            .output_frames = cap_frames,
            .src_ratio = g_upsample_ratio,
            .end_of_input = 0,
        };
        if (src_process(g_src, &data) == 0) {
            int n = (int)data.output_frames_gen * 2;
            if (n > max_samples) n = max_samples & ~1;
            for (int i = 0; i < n; i++) {
                float v = output[i] * 32768.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                out[i] = (int16_t)v;
            }
            return n;
        }
    }
    memcpy(out, lr, 4 * sizeof(int16_t));
    return 4;
}

static int audio_resample_sep(const int16_t mono[2], int16_t *out, int max_out) {
    if (max_out <= 0) return 0;
    if (audio_ratio_is_unity()) {
        int n = max_out < 2 ? max_out : 2;
        memcpy(out, mono, (size_t)n * sizeof(int16_t));
        return n;
    }
    int16_t lr[4] = { mono[0], mono[0], mono[1], mono[1] };
    int16_t stereo[32];
    int ns = audio_resample_sep_stereo(lr, stereo, (int)(sizeof(stereo) / sizeof(stereo[0])));
    int n_out = 0;
    for (int i = 0; i + 1 < ns && n_out < max_out; i += 2)
        out[n_out++] = (int16_t)(((int32_t)stereo[i] + stereo[i + 1]) / 2);
    return n_out;
}

// forward declaration de la versión PipeWire (definición más abajo)
static int audio_pw_open(void);
static void audio_pw_close(void);
static void audio_feed_sep_pw(const uint8_t* payload);
static int audio_writer_start(void);
static void audio_writer_stop(void);

static void audio_open(const char* pcm_name) {
    // HD60S_AUDIO_PW=1 ramifica a la implementación nativa PipeWire
    const char* env_pw = getenv("HD60S_AUDIO_PW");
    if (env_pw && env_pw[0] && env_pw[0] != '0' && env_pw[0] != 'n') {
        if (audio_pw_open() == 0) {
            g_use_pw = 1;
            return;
        }
        fprintf(stderr, "[audio] PipeWire unavailable; falling back to ALSA %s\n", pcm_name);
        g_use_pw = 0;
    }
    int err = snd_pcm_open(&g_pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "[audio] snd_pcm_open(%s) failed: %s\n", pcm_name, snd_strerror(err));
        g_pcm = NULL;
        return;
    }
    err = snd_pcm_set_params(g_pcm,
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_ACCESS_RW_INTERLEAVED,
        1,          // mono
        AUDIO_TARGET_RATE,
        1,          // soft resample
        80000);     // 80 ms
    if (err < 0) {
        fprintf(stderr, "[audio] snd_pcm_set_params failed: %s\n", snd_strerror(err));
        snd_pcm_close(g_pcm); g_pcm = NULL;
        return;
    }
    audio_src_init();
    fprintf(stderr, "[audio] ALSA %s opened (48kHz S16_LE mono; SEP stereo downmix)\n", pcm_name);
    audio_writer_start();
}

static void audio_write_samples(const int16_t* samples, int frames) {
    if (!g_pcm || !samples || frames <= 0) return;
    int offset = 0;
    while (offset < frames) {
        snd_pcm_sframes_t written = snd_pcm_writei(g_pcm, samples + offset,
                                                   frames - offset);
        if (written < 0) {
            g_audio_underrun++;
            int err = snd_pcm_recover(g_pcm, (int)written, 1);
            if (err < 0) {
                fprintf(stderr, "[audio] recover failed: %s\n", snd_strerror(err));
                break;
            }
            continue;
        }
        if (written == 0) break;
        offset += (int)written;
    }
    g_audio_frames += (unsigned long long)offset;
    g_audio_samples_out += (unsigned long long)offset;
}

static void audio_writer_signal(void) {
    pthread_mutex_lock(&g_audio_writer_wake_mutex);
    pthread_cond_signal(&g_audio_writer_wake_cond);
    pthread_mutex_unlock(&g_audio_writer_wake_mutex);
}

static int audio_queue_push(const int16_t* samples, int frames) {
    uint32_t head = atomic_load_explicit(&g_audio_queue_head, memory_order_relaxed);
    uint32_t tail = atomic_load_explicit(&g_audio_queue_tail, memory_order_acquire);
    if (head - tail >= AUDIO_QUEUE_BATCHES) {
        atomic_fetch_add_explicit(&g_audio_queue_drops, 1, memory_order_relaxed);
        return -1;
    }
    uint32_t slot = head % AUDIO_QUEUE_BATCHES;
    memcpy(g_audio_queue[slot], samples, (size_t)frames * sizeof(samples[0]));
    g_audio_queue_len[slot] = (uint16_t)frames;
    atomic_store_explicit(&g_audio_queue_head, head + 1, memory_order_release);
    audio_writer_signal();
    return 0;
}

static void* audio_writer_main(void* unused) {
    (void)unused;
    for (;;) {
        uint32_t tail = atomic_load_explicit(&g_audio_queue_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&g_audio_queue_head, memory_order_acquire);
        if (tail != head) {
            uint32_t slot = tail % AUDIO_QUEUE_BATCHES;
            audio_write_samples(g_audio_queue[slot], g_audio_queue_len[slot]);
            atomic_store_explicit(&g_audio_queue_tail, tail + 1, memory_order_release);
            continue;
        }

        pthread_mutex_lock(&g_audio_writer_wake_mutex);
        while (!atomic_load_explicit(&g_audio_writer_stop, memory_order_acquire) &&
               atomic_load_explicit(&g_audio_queue_head, memory_order_acquire) ==
                   atomic_load_explicit(&g_audio_queue_tail, memory_order_relaxed)) {
            pthread_cond_wait(&g_audio_writer_wake_cond, &g_audio_writer_wake_mutex);
        }
        int stop = atomic_load_explicit(&g_audio_writer_stop, memory_order_acquire);
        pthread_mutex_unlock(&g_audio_writer_wake_mutex);

        if (stop &&
            atomic_load_explicit(&g_audio_queue_head, memory_order_acquire) ==
                atomic_load_explicit(&g_audio_queue_tail, memory_order_relaxed))
            break;
    }
    return NULL;
}

static int audio_writer_start(void) {
    atomic_store_explicit(&g_audio_queue_head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_audio_queue_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&g_audio_writer_stop, 0, memory_order_relaxed);
    int rc = pthread_create(&g_audio_writer_thread, NULL, audio_writer_main, NULL);
    if (rc != 0) {
        fprintf(stderr, "[audio] async ALSA writer unavailable: %s; using direct fallback\n",
                strerror(rc));
        g_audio_writer_started = 0;
        return -1;
    }
    g_audio_writer_started = 1;
    fprintf(stderr, "[audio] async ALSA writer started (USB callback remains non-blocking)\n");
    return 0;
}

static void audio_writer_stop(void) {
    if (!g_audio_writer_started) return;
    atomic_store_explicit(&g_audio_writer_stop, 1, memory_order_release);
    audio_writer_signal();
    pthread_join(g_audio_writer_thread, NULL);
    g_audio_writer_started = 0;
}

static void audio_flush(void) {
    if (g_audio_buf_pos == 0) return;
    if (g_pcm) {
        if (g_audio_writer_started)
            audio_queue_push(g_audio_buf, g_audio_buf_pos);
        else
            audio_write_samples(g_audio_buf, g_audio_buf_pos);
    }
    g_audio_buf_pos = 0;
}

static void audio_feed_sep(const uint8_t* payload) {
    g_audio_packets++;
    g_audio_bytes += 8;

    int16_t mono[2];
    decode_sep_mono(payload, mono);
    int is_active = (abs(mono[0]) > 32 || abs(mono[1]) > 32);

    static int g_sep_diag_count = 0;
    if (g_sep_diag_count < 10 || (is_active && g_sep_diag_count < 20)) {
        int16_t lr[4];
        decode_sep_stereo(payload, lr);
        fprintf(stderr, "[sep-raw #%d] L0=%d R0=%d L1=%d R1=%d active=%d\n",
                g_sep_diag_count, lr[0], lr[1], lr[2], lr[3], is_active);
        g_sep_diag_count++;
    }

    if (g_use_pw) { audio_feed_sep_pw(payload); return; }

    audio_track_cadence_and_recalibrate(is_active);

    if (!g_pcm || g_upsample_ratio <= 0) return;

    int16_t batch[32];
    int n = audio_resample_sep(mono, batch, (int)(sizeof(batch) / sizeof(batch[0])));
    for (int i = 0; i < n; i++) {
        if (g_audio_buf_pos >= AUDIO_BATCH_FRAMES) audio_flush();
        g_audio_buf[g_audio_buf_pos++] = batch[i];
    }
    if (g_audio_buf_pos >= AUDIO_BATCH_FRAMES) audio_flush();
}

// ======================================================================
// 🔥 implementación nativa libpipewire (propuesta Opus 4.8, 2026-07-11)
// antes: iso_capture → snd_aloop → arecord|aplay → PipeWire = 4 etapas de buffer
// ahora: iso_capture → pw_stream (directo) = 1 etapa, ~2.6ms/quantum
// se activa con env HD60S_AUDIO_PW=1. Flag para A/B frente a la impl ALSA.
// ======================================================================
static struct pw_thread_loop* g_pw_loop = NULL;
static struct pw_stream* g_pw_stream = NULL;
static int g_pw_initialized = 0;
static int g_pw_started = 0;
// ring buffer (mono int16 samples). Producer: iso_capture (vía audio_feed_sep).
// Consumer: callback process de pipewire (otro hilo).
// 🔥 test de kusq: 683ms es demasiado; se acumula audio viejo → latencia.
// recortado a 4096 samples @ 96kHz = 43ms; si está lleno, tira lo viejo (prioriza lo nuevo).
#define PW_CHANNELS 2
#define PW_RING_SIZE 16384
#define PW_TARGET_FILL 2048  /* ~43 ms @ 48 kHz: absorbe jitter USB */
// 2026-07-18 lock-free SPSC ring: el producer solo toca head, el consumer solo tail.
// sin pthread_mutex se elimina la priority inversion (el hilo RT esperando el mutex del producer).
// ordering release/acquire garantiza que los samples escritos se ven cross-thread.
// alignas(64): evita false sharing (si producer/consumer van en CPU distintas, head y tail
// en la misma cache line invalidan al otro en cada write = se pierde el mérito SPSC).
alignas(64) static _Atomic uint32_t g_pw_ring_head = 0;  // solo producer (hilo iso)
alignas(64) static _Atomic uint32_t g_pw_ring_tail = 0;  // solo consumer (hilo pw)
alignas(64) static int16_t g_pw_ring[PW_RING_SIZE];
// drop counters (para diagnóstico; a 24k SEP/s el coste de atomic incr es irrelevante)
static _Atomic uint64_t g_pw_drops_old = 0;   // samples que el consumer hizo fast-forward
static _Atomic uint64_t g_pw_drops_new = 0;   // samples que el producer no pudo escribir (full)
static _Atomic uint64_t g_pw_process_calls = 0;
static _Atomic uint64_t g_pw_output_writes = 0;
static _Atomic uint64_t g_pw_output_frames = 0;
static _Atomic uint64_t g_pw_output_errors = 0;
static _Atomic uint32_t g_pw_fill_min = UINT32_MAX;
static _Atomic uint32_t g_pw_fill_max = 0;

static int16_t g_pw_last_l = 0;
static int16_t g_pw_last_r = 0;

// callback process de pw_stream: lo llama el hilo RT de PipeWire. Dequeue del ring
// y escribe en el buffer PW.
// 🔥 anti-clipping: en underflow no rellenar con 0; decay de last_sample
// 🔥 2026-07-18 lock-free SPSC: no pelea el mutex con el producer. El drop-old
//    queda en el consumer (para respetar la regla SPSC de un solo escritor de tail).
static void pw_on_process(void* userdata) {
    (void)userdata;
    atomic_fetch_add_explicit(&g_pw_process_calls, 1, memory_order_relaxed);
    struct pw_buffer* b = pw_stream_dequeue_buffer(g_pw_stream);
    if (!b) {
        atomic_fetch_add_explicit(&g_pw_output_errors, 1, memory_order_relaxed);
        return;
    }
    struct spa_buffer* buf = b->buffer;
    if (!buf->datas[0].data) {
        atomic_fetch_add_explicit(&g_pw_output_errors, 1, memory_order_relaxed);
        pw_stream_queue_buffer(g_pw_stream, b);
        return;
    }

    int16_t* dst = (int16_t*)buf->datas[0].data;
    uint32_t max_frames = buf->datas[0].maxsize / (PW_CHANNELS * sizeof(int16_t));
    uint32_t want = b->requested;
    if (want == 0) want = PW_TARGET_FILL;
    if (want > max_frames) want = max_frames;

    uint32_t head = atomic_load_explicit(&g_pw_ring_head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&g_pw_ring_tail, memory_order_relaxed);
    uint32_t avail_s = (head - tail) & (PW_RING_SIZE - 1);
    avail_s &= ~(uint32_t)(PW_CHANNELS - 1);
    uint32_t avail = avail_s / PW_CHANNELS;
    uint32_t old_min = atomic_load_explicit(&g_pw_fill_min, memory_order_relaxed);
    while (avail < old_min && !atomic_compare_exchange_weak_explicit(
        &g_pw_fill_min, &old_min, avail, memory_order_relaxed, memory_order_relaxed)) {}
    uint32_t old_max = atomic_load_explicit(&g_pw_fill_max, memory_order_relaxed);
    while (avail > old_max && !atomic_compare_exchange_weak_explicit(
        &g_pw_fill_max, &old_max, avail, memory_order_relaxed, memory_order_relaxed)) {}

    if (avail > PW_TARGET_FILL * 3) {
        uint32_t drop = 64;
        tail = (tail + drop * PW_CHANNELS) & (PW_RING_SIZE - 1);
        avail_s = (head - tail) & (PW_RING_SIZE - 1);
        avail_s &= ~(uint32_t)(PW_CHANNELS - 1);
        avail = avail_s / PW_CHANNELS;
        atomic_fetch_add_explicit(&g_pw_drops_old, drop, memory_order_relaxed);
    }

    uint32_t n = (want < avail) ? want : avail;
    if (n < want) g_audio_pw_underflow++;
    for (uint32_t i = 0; i < n; i++) {
        dst[i * 2]     = g_pw_ring[(tail + i * 2) & (PW_RING_SIZE - 1)];
        dst[i * 2 + 1] = g_pw_ring[(tail + i * 2 + 1) & (PW_RING_SIZE - 1)];
    }
    if (n > 0) {
        g_pw_last_l = dst[(n - 1) * 2];
        g_pw_last_r = dst[(n - 1) * 2 + 1];
    }
    g_audio_samples_out += n;

    /* No decaer a silencio: un hueco corto se oye como corte seco. */
    for (uint32_t i = n; i < want; i++) {
        dst[i * 2] = g_pw_last_l;
        dst[i * 2 + 1] = g_pw_last_r;
    }

    atomic_store_explicit(&g_pw_ring_tail, (tail + n * PW_CHANNELS) & (PW_RING_SIZE - 1),
                          memory_order_release);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = (int32_t)(PW_CHANNELS * sizeof(int16_t));
    buf->datas[0].chunk->size = want * PW_CHANNELS * sizeof(int16_t);
    atomic_fetch_add_explicit(&g_pw_output_writes, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_pw_output_frames, want, memory_order_relaxed);
    pw_stream_queue_buffer(g_pw_stream, b);
}

static const struct pw_stream_events g_pw_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = pw_on_process,
};

// escribe samples al ring en batch (desde el hilo iso)
// 🔥 2026-07-18 lock-free SPSC + batch: se quita el mutex per-sample de antes.
//    si full, drop-new (no escribe). El drop-old vive en el consumer (pw_on_process).
static void pw_ring_push_batch(const int16_t* samples, int n) {
    if (n <= 0) return;
    // relaxed: head es write solo mío; aquí basta con el valor más reciente
    uint32_t head = atomic_load_explicit(&g_pw_ring_head, memory_order_relaxed);
    // acquire: para ver el space que el consumer liberó al avanzar tail
    uint32_t tail = atomic_load_explicit(&g_pw_ring_tail, memory_order_acquire);
    uint32_t fill = (head - tail) & (PW_RING_SIZE - 1);
    uint32_t space = PW_RING_SIZE - 1 - fill;   // -1 para distinguir full/empty
    int to_write = (n < (int)space) ? n : (int)space;
    int dropped = n - to_write;
    g_audio_samples_in += (unsigned long long)n;
    if (dropped > 0) {
        atomic_fetch_add_explicit(&g_pw_drops_new, (uint64_t)dropped, memory_order_relaxed);
    }
    for (int i = 0; i < to_write; i++) {
        g_pw_ring[(head + i) & (PW_RING_SIZE - 1)] = samples[i];
    }
    // release: cuando el consumer ve el head nuevo, también ve los samples escritos
    atomic_store_explicit(&g_pw_ring_head, (head + to_write) & (PW_RING_SIZE - 1),
                          memory_order_release);
}

static int audio_pw_open(void) {
    pw_init(NULL, NULL);
    g_pw_initialized = 1;
    g_pw_loop = pw_thread_loop_new("hd60s-audio", NULL);
    if (!g_pw_loop) {
        fprintf(stderr, "[audio-pw] pw_thread_loop_new failed\n");
        pw_deinit();
        g_pw_initialized = 0;
        return -1;
    }
    pw_thread_loop_lock(g_pw_loop);

    /* Reproduce hacia un sink permanente (hd60s_out). OBS captura el
     * monitor remapeado a hd60s_capture, que no desaparece si iso_capture
     * se reinicia. HD60S_PW_SINK=source vuelve al nodo nativo. */
    const char *pw_sink = getenv("HD60S_PW_SINK");
    if (!pw_sink || !pw_sink[0])
        pw_sink = "hd60s_out";
    int as_source = (strcmp(pw_sink, "source") == 0 || strcmp(pw_sink, "-") == 0);

    struct pw_properties* props;
    if (as_source) {
        props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Game",
            PW_KEY_MEDIA_NAME, "hd60s_capture",
            PW_KEY_NODE_NAME, "hd60s_capture",
            PW_KEY_NODE_DESCRIPTION, "Elgato HD60 S Audio Capture",
            PW_KEY_NODE_LATENCY, "128/48000",
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            NULL);
    } else {
        props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Game",
            PW_KEY_MEDIA_CLASS, "Stream/Output/Audio",
            PW_KEY_MEDIA_NAME, "hd60s_play",
            PW_KEY_NODE_NAME, "hd60s_play",
            PW_KEY_NODE_DESCRIPTION, "Elgato HD60 S playback",
            PW_KEY_TARGET_OBJECT, pw_sink,
            PW_KEY_NODE_LATENCY, "256/48000",
            PW_KEY_NODE_ALWAYS_PROCESS, "true",
            NULL);
    }

    g_pw_stream = pw_stream_new_simple(
        pw_thread_loop_get_loop(g_pw_loop),
        as_source ? "hd60s_capture" : "hd60s_play",
        props,
        &g_pw_events,
        NULL);

    if (!g_pw_stream) {
        fprintf(stderr, "[audio-pw] pw_stream_new_simple failed\n");
        pw_thread_loop_unlock(g_pw_loop);
        pw_thread_loop_destroy(g_pw_loop);
        g_pw_loop = NULL;
        pw_deinit();
        g_pw_initialized = 0;
        return -1;
    }

    // 2026-07-21 P3-1: rate del stream PW unificado a 48kHz. Coincide con el graph rate,
    // así que desaparece el resample interno 96→48 de PipeWire. Se mata al culpable del
    // coloreado de agudos y jitter («popopó»). En iso_capture, libsamplerate SINC hace el
    // downsample Switch 96kHz → 48kHz con anti-alias.
    uint8_t buffer[1024];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    struct spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .channels = 2,
        .rate = 48000,
        .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR }
    );
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&pod_builder, SPA_PARAM_EnumFormat, &info);

    int r = pw_stream_connect(g_pw_stream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS,
        params, 1);
    if (r < 0) {
        fprintf(stderr, "[audio-pw] pw_stream_connect failed: %d\n", r);
        pw_stream_destroy(g_pw_stream);
        g_pw_stream = NULL;
        pw_thread_loop_unlock(g_pw_loop);
        pw_thread_loop_destroy(g_pw_loop);
        g_pw_loop = NULL;
        pw_deinit();
        g_pw_initialized = 0;
        return -1;
    }

    pw_thread_loop_unlock(g_pw_loop);
    r = pw_thread_loop_start(g_pw_loop);
    if (r < 0) {
        fprintf(stderr, "[audio-pw] pw_thread_loop_start failed: %d\n", r);
        pw_stream_disconnect(g_pw_stream);
        pw_stream_destroy(g_pw_stream);
        g_pw_stream = NULL;
        pw_thread_loop_destroy(g_pw_loop);
        g_pw_loop = NULL;
        pw_deinit();
        g_pw_initialized = 0;
        return -1;
    }
    g_pw_started = 1;
    if (as_source)
        fprintf(stderr, "[audio-pw] native source hd60s_capture started (48kHz S16_LE stereo)\n");
    else
        fprintf(stderr, "[audio-pw] playback → %s (48kHz S16_LE stereo); OBS lee hd60s_capture\n",
                pw_sink);
    audio_src_init();
    return 0;
}

static void audio_pw_close(void) {
    if (g_pw_started && g_pw_loop) {
        pw_thread_loop_stop(g_pw_loop);
        g_pw_started = 0;
    }
    if (g_pw_stream) {
        pw_stream_disconnect(g_pw_stream);
        pw_stream_destroy(g_pw_stream);
        g_pw_stream = NULL;
    }
    if (g_pw_loop) {
        pw_thread_loop_destroy(g_pw_loop);
        g_pw_loop = NULL;
    }
    audio_src_close();
    if (g_pw_initialized) {
        pw_deinit();
        g_pw_initialized = 0;
    }
}

// versión PipeWire de audio_feed_sep: push al ring de los samples upsampleados
static void audio_feed_sep_pw(const uint8_t* payload) {
    int16_t lr[4];
    decode_sep_stereo(payload, lr);
    int is_active = (abs(lr[0]) > 32 || abs(lr[1]) > 32 || abs(lr[2]) > 32 || abs(lr[3]) > 32);

    audio_track_cadence_and_recalibrate(is_active);
    if (!g_pw_stream || g_upsample_ratio <= 0) return;

    // 2026-07-19 PLL update: cada 1s ajusta upsample_ratio a partir del error de fill del ring.
    // adaptive rate follow que absorbe el empeoramiento lento por drift.
    // ganancia: 100ppm drift = 9.6 samples/s de desvío; con Kp=1e-6, Ki=1e-7 sigue en 30-60s.
    // ⚠️ 2026-07-19 escucha kusq: al cambiar el ratio al vuelo el PLL produce un efecto «algo disonante»
    //    (el tono se mueve). Default off; opt-in con HD60S_PLL_ENABLE=1.
    static int pll_checked = 0, pll_enabled = 0;
    if (!pll_checked) {
        pll_enabled = getenv("HD60S_PLL_ENABLE") ? 1 : 0;
        pll_checked = 1;
    }
    if (pll_enabled) {
        double now = now_monotonic_s();
        double dt = now - g_pll_last_update;
        if (dt >= 1.0) {
            g_pll_last_update = now;
            uint32_t head = atomic_load_explicit(&g_pw_ring_head, memory_order_relaxed);
            uint32_t tail = atomic_load_explicit(&g_pw_ring_tail, memory_order_relaxed);
            int32_t fill = (int32_t)(((head - tail) & (PW_RING_SIZE - 1)) / PW_CHANNELS);
            int32_t err = fill - (int32_t)PW_TARGET_FILL;
            g_pll_integral += (double)err * dt;
            // clamp anti-runaway del integrador
            if (g_pll_integral > 100000.0) g_pll_integral = 100000.0;
            if (g_pll_integral < -100000.0) g_pll_integral = -100000.0;
            double kp = 1e-6, ki = 1e-7;
            double adjust = kp * (double)err + ki * g_pll_integral;
            // clamp de la corrección por paso (anti-oscilación)
            if (adjust > 5e-4) adjust = 5e-4;
            if (adjust < -5e-4) adjust = -5e-4;
            // bajar ratio = menos samples de salida = si está lleno (err>0) se baja
            double new_ratio = g_upsample_ratio - adjust;
            // tope absoluto ±5% respecto a la base (snap 95kHz base=1.000, rango [0.95, 1.05])
            double lo = g_pll_base_ratio * 0.95;
            double hi = g_pll_base_ratio * 1.05;
            if (new_ratio < lo) new_ratio = lo;
            if (new_ratio > hi) new_ratio = hi;
            g_upsample_ratio = new_ratio;
            g_pll_updates++;
            if (hd60s_env_present("HD60S_PLL_DEBUG")) {
                fprintf(stderr, "[pll] #%llu fill=%d err=%+d int=%+.1f adj=%+.6f ratio=%.6f\n",
                        (unsigned long long)g_pll_updates, fill, err,
                        g_pll_integral, adjust, g_upsample_ratio);
            }
        }
    }

    int16_t batch[32];
    int n = audio_resample_sep_stereo(lr, batch, (int)(sizeof(batch) / sizeof(batch[0])));
    pw_ring_push_batch(batch, n);
}


static void audio_close(void) {
    if (g_use_pw) {
        audio_pw_close();
        return;
    }
    if (!g_pcm) return;
    audio_flush();
    audio_writer_stop();
    snd_pcm_drain(g_pcm);
    snd_pcm_close(g_pcm);
    g_pcm = NULL;
    audio_src_close();
}


void hd60s_audio_open(const char *alsa_dev) {
    audio_open(alsa_dev);
}

void hd60s_audio_feed_sep(const uint8_t *payload12) {
    audio_feed_sep(payload12);
}

void hd60s_audio_close(void) {
    audio_close();
}

unsigned long long hd60s_audio_packets(void) {
    return atomic_load_explicit(&g_audio_packets, memory_order_relaxed);
}

void hd60s_audio_dump_stats(FILE *fp) {
    if (!fp) fp = stderr;
    fprintf(fp, "audio:     packets=%llu bytes=%llu samples_in=%llu samples_out=%llu "
                    "underrun=%llu pw_underflow=%llu\n",
            atomic_load_explicit(&g_audio_packets, memory_order_relaxed),
            atomic_load_explicit(&g_audio_bytes, memory_order_relaxed),
            atomic_load_explicit(&g_audio_samples_in, memory_order_relaxed),
            atomic_load_explicit(&g_audio_samples_out, memory_order_relaxed),
            atomic_load_explicit(&g_audio_underrun, memory_order_relaxed),
            atomic_load_explicit(&g_audio_pw_underflow, memory_order_relaxed));
    uint64_t drops_old = atomic_load_explicit(&g_pw_drops_old, memory_order_relaxed);
    uint64_t drops_new = atomic_load_explicit(&g_pw_drops_new, memory_order_relaxed);
    fprintf(fp, "pw ring:   drops_old=%llu (consumer fast-forward) drops_new=%llu (producer full)\n",
            (unsigned long long)drops_old, (unsigned long long)drops_new);
    fprintf(fp, "pw output: process_calls=%llu writes=%llu frames=%llu errors=%llu fill_min=%u fill_max=%u\n",
            (unsigned long long)atomic_load_explicit(&g_pw_process_calls, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_pw_output_writes, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_pw_output_frames, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&g_pw_output_errors, memory_order_relaxed),
            atomic_load_explicit(&g_pw_fill_min, memory_order_relaxed),
            atomic_load_explicit(&g_pw_fill_max, memory_order_relaxed));
}
