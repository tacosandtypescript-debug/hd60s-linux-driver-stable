#include "hd60s_pace.h"
#include "hd60s_util.h"
#include "hd60s_v4l2.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#define FRAME_W 1920
#define FRAME_H 1080
#define LINE_BYTES (FRAME_W * 2)
#define FRAME_BYTES (LINE_BYTES * FRAME_H)

int g_pace_output = 0;
uint8_t g_pace_frame[FRAME_BYTES];
uint8_t g_pace_queue[PACE_QUEUE_DEPTH][FRAME_BYTES];
unsigned long long g_pace_queue_seq[PACE_QUEUE_DEPTH];
unsigned int g_pace_queue_head = 0;
unsigned int g_pace_queue_tail = 0;
unsigned int g_pace_queue_count = 0;
int g_pace_have_frame = 0;
uint64_t g_pace_next_ns = 0;
unsigned long long g_v4l_write_seq = 0;
unsigned long long g_pace_frame_seq = 0;
unsigned long long g_pace_last_written_seq = 0;

// Temporary cadence diagnostics.  These counters do not alter capture logic.
int g_diag = 0;
uint64_t g_diag_window_ns = 0, g_diag_frame_start_ns = 0;
uint64_t g_diag_last_write_ns = 0;
unsigned long long g_diag_blk = 0, g_diag_complete = 0;
unsigned long long g_diag_partial = 0, g_diag_resets = 0;
unsigned long long g_diag_writes = 0, g_diag_write_fail = 0;
unsigned long long g_diag_discarded = 0;
unsigned long long g_diag_paced_new = 0, g_diag_paced_repeat = 0;
unsigned long long g_diag_paced_wait = 0;
unsigned long long g_diag_pace_queue_drops = 0;
unsigned long long g_diag_interval_n = 0, g_diag_latency_n = 0, g_diag_write_dur_n = 0;
unsigned long long g_diag_markers = 0, g_diag_q_bad = 0;
unsigned long long g_diag_act = 0;
unsigned long long g_diag_input_bytes = 0;
size_t g_diag_q_min = SIZE_MAX, g_diag_q_max = 0;
uint64_t g_diag_interval_min = UINT64_MAX, g_diag_interval_max = 0;
uint64_t g_diag_latency_min = UINT64_MAX, g_diag_latency_max = 0;
uint64_t g_diag_write_dur_min = UINT64_MAX, g_diag_write_dur_max = 0;
long double g_diag_interval_sum = 0, g_diag_latency_sum = 0, g_diag_write_dur_sum = 0;

void hd60s_diag_report_if_due(void) {
    if (!g_diag) return;
    uint64_t now = now_mono_ns();
    if (!g_diag_window_ns) { g_diag_window_ns = now; return; }
    if (now - g_diag_window_ns < 1000000000ull) return;
    double sec = (double)(now - g_diag_window_ns) / 1e9;
    fprintf(stderr,
            "[cadence] %.3fs BLK=%llu complete=%llu partial=%llu resets=%llu discarded=%llu "
            "v4l_write=%llu fail=%llu fps_complete=%.3f fps_write=%.3f "
            "paced_new=%llu paced_repeat=%llu paced_wait=%llu queue_drop=%llu "
            "write_dt_us[min/avg/max]=%llu/%.1f/%llu latency_us[min/avg/max]=%llu/%.1f/%llu "
            "write_call_us[min/avg/max]=%llu/%.1f/%llu markers[A/B]=%llu/%llu "
            "marker_q[min/max/bad]=%zu/%zu/%llu input_MBps=%.2f\n",
            sec, g_diag_blk, g_diag_complete, g_diag_partial, g_diag_resets,
            g_diag_discarded,
            g_diag_writes, g_diag_write_fail,
            g_diag_complete / sec, g_diag_writes / sec,
            g_diag_paced_new, g_diag_paced_repeat, g_diag_paced_wait,
            g_diag_pace_queue_drops,
            g_diag_interval_n ? (unsigned long long)(g_diag_interval_min / 1000) : 0,
            g_diag_interval_n ? (double)(g_diag_interval_sum / g_diag_interval_n / 1000.0) : 0.0,
            g_diag_interval_n ? (unsigned long long)(g_diag_interval_max / 1000) : 0,
            g_diag_latency_n ? (unsigned long long)(g_diag_latency_min / 1000) : 0,
            g_diag_latency_n ? (double)(g_diag_latency_sum / g_diag_latency_n / 1000.0) : 0.0,
            g_diag_latency_n ? (unsigned long long)(g_diag_latency_max / 1000) : 0,
            g_diag_write_dur_n ? (unsigned long long)(g_diag_write_dur_min / 1000) : 0,
            g_diag_write_dur_n ? (double)(g_diag_write_dur_sum / g_diag_write_dur_n / 1000.0) : 0.0,
            g_diag_write_dur_n ? (unsigned long long)(g_diag_write_dur_max / 1000) : 0,
            g_diag_act, g_diag_blk, g_diag_markers ? g_diag_q_min : 0,
            g_diag_markers ? g_diag_q_max : 0, g_diag_q_bad,
            (double)g_diag_input_bytes / sec / 1000000.0);
    g_diag_window_ns = now;
    g_diag_blk = g_diag_complete = g_diag_partial = g_diag_resets = 0;
    g_diag_writes = g_diag_write_fail = 0;
    g_diag_discarded = 0;
    g_diag_paced_new = g_diag_paced_repeat = g_diag_paced_wait = 0;
    g_diag_pace_queue_drops = 0;
    g_diag_interval_n = g_diag_latency_n = g_diag_write_dur_n = 0;
    g_diag_markers = g_diag_q_bad = 0;
    g_diag_act = 0;
    g_diag_input_bytes = 0;
    g_diag_q_min = SIZE_MAX; g_diag_q_max = 0;
    g_diag_interval_min = g_diag_latency_min = UINT64_MAX;
    g_diag_interval_max = g_diag_latency_max = 0;
    g_diag_interval_sum = g_diag_latency_sum = g_diag_write_dur_sum = 0;
}

void hd60s_pace_configure(int enabled) {
    g_pace_output = enabled;
}

void hd60s_diag_set(int on) {
    g_diag = on;
}

void hd60s_pace_push_frame(const uint8_t *frame, unsigned long long seq) {
    if (g_pace_queue_count == PACE_QUEUE_DEPTH) {
        g_pace_queue_tail = (g_pace_queue_tail + 1) % PACE_QUEUE_DEPTH;
        g_pace_queue_count--;
        if (g_diag) g_diag_pace_queue_drops++;
    }
    memcpy(g_pace_queue[g_pace_queue_head], frame, FRAME_BYTES);
    g_pace_queue_seq[g_pace_queue_head] = seq;
    g_pace_queue_head = (g_pace_queue_head + 1) % PACE_QUEUE_DEPTH;
    g_pace_queue_count++;
}

void hd60s_pace_output_if_due(void) {
    if (!g_pace_output || !hd60s_v4l2_is_open()) return;
    if (g_pace_queue_count == 0) {
        // Never publish the previous frame a second time.  A small source /
        // pacing-clock drift can temporarily empty the queue even though the
        // capture is healthy; waiting for the next complete frame preserves
        // frame identity and lets the consumer hold its last image naturally.
        if (g_pace_have_frame && g_diag) g_diag_paced_wait++;
        return;
    }
    const uint64_t period = 16666667ull; // 60 Hz
    uint64_t now = now_mono_ns();
    if (!g_pace_next_ns) g_pace_next_ns = now;
    if (now < g_pace_next_ns) return;
    const uint8_t* frame = g_pace_queue[g_pace_queue_tail];
    unsigned long long frame_seq = g_pace_queue_seq[g_pace_queue_tail];
    uint64_t write_start_ns = now;
    ssize_t w = hd60s_v4l2_write_frame(frame, FRAME_BYTES);
    uint64_t write_done_ns = now_mono_ns();
    fprintf(stderr, "[v4l-write] seq=%llu mode=paced bytes=%zd expected=%zu %s\n",
            ++g_v4l_write_seq, w, (size_t)FRAME_BYTES,
            w == FRAME_BYTES ? "OK" : "ANOMALY");
    if (g_diag) {
        uint64_t dur = write_done_ns - write_start_ns;
        g_diag_write_dur_n++;
        g_diag_write_dur_sum += dur;
        if (dur < g_diag_write_dur_min) g_diag_write_dur_min = dur;
        if (dur > g_diag_write_dur_max) g_diag_write_dur_max = dur;
    }
    if (w == FRAME_BYTES) {
        if (g_diag) {
            if (frame_seq == g_pace_last_written_seq)
                g_diag_paced_repeat++;
            else
                g_diag_paced_new++;
        }
        g_pace_last_written_seq = frame_seq;
        memcpy(g_pace_frame, frame, FRAME_BYTES);
        g_pace_frame_seq = frame_seq;
        g_pace_have_frame = 1;
        g_pace_queue_tail = (g_pace_queue_tail + 1) % PACE_QUEUE_DEPTH;
        g_pace_queue_count--;
        g_diag_writes++;
        if (g_diag_last_write_ns) {
            uint64_t dt = write_done_ns - g_diag_last_write_ns;
            g_diag_interval_n++; g_diag_interval_sum += dt;
            if (dt < g_diag_interval_min) g_diag_interval_min = dt;
            if (dt > g_diag_interval_max) g_diag_interval_max = dt;
        }
        g_diag_last_write_ns = write_done_ns;
    } else {
        g_diag_write_fail++;
    }
    do { g_pace_next_ns += period; } while (g_pace_next_ns <= write_done_ns);
    hd60s_diag_report_if_due();
}
