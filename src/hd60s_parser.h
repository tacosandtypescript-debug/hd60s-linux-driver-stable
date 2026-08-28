#ifndef HD60S_PARSER_H
#define HD60S_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include <libusb-1.0/libusb.h>

extern unsigned long long g_frames_out;
extern unsigned long long g_resyncs;
extern unsigned long long g_resync_empty;
extern unsigned long long g_resync_marker;
extern unsigned long long g_resync_overflow;
extern int g_trace_enabled;
extern unsigned long long g_trace_callback_no;

void hd60s_parser_feed(const uint8_t *data, size_t len);
void hd60s_parser_notify_loss(size_t bytes_lost);
void hd60s_parser_reset(const char *why);
void hd60s_parser_reset_video_phase(void);
void hd60s_parser_trace_init(void);
void hd60s_parser_trace_begin(unsigned long long callback_no, int packet_index,
                              int transfer_status,
                              const struct libusb_iso_packet_descriptor *d,
                              const uint8_t *data);
void hd60s_parser_trace_end(void);

#endif
