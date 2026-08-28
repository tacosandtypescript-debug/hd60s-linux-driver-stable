#ifndef HD60S_PACE_H
#define HD60S_PACE_H

#include <stddef.h>
#include <stdint.h>

#define PACE_QUEUE_DEPTH 4

extern int g_pace_output;
extern unsigned long long g_v4l_write_seq;
extern int g_diag;
extern uint64_t g_diag_window_ns, g_diag_frame_start_ns;
extern uint64_t g_diag_last_write_ns;
extern unsigned long long g_diag_blk, g_diag_complete;
extern unsigned long long g_diag_partial, g_diag_resets;
extern unsigned long long g_diag_writes, g_diag_write_fail;
extern unsigned long long g_diag_discarded;
extern unsigned long long g_diag_paced_new, g_diag_paced_repeat;
extern unsigned long long g_diag_paced_wait;
extern unsigned long long g_diag_pace_queue_drops;
extern unsigned long long g_diag_interval_n, g_diag_latency_n, g_diag_write_dur_n;
extern unsigned long long g_diag_markers, g_diag_q_bad;
extern unsigned long long g_diag_act;
extern unsigned long long g_diag_input_bytes;
extern size_t g_diag_q_min, g_diag_q_max;
extern uint64_t g_diag_interval_min, g_diag_interval_max;
extern uint64_t g_diag_latency_min, g_diag_latency_max;
extern uint64_t g_diag_write_dur_min, g_diag_write_dur_max;
extern long double g_diag_interval_sum, g_diag_latency_sum, g_diag_write_dur_sum;

void hd60s_pace_configure(int enabled);
void hd60s_pace_push_frame(const uint8_t *frame, unsigned long long seq);
void hd60s_pace_output_if_due(void);
void hd60s_diag_set(int on);
void hd60s_diag_report_if_due(void);

#endif
