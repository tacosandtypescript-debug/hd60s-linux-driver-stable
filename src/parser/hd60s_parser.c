#include "hd60s_parser.h"
#include "hd60s_util.h"
#include "hd60s_v4l2.h"
#include "hd60s_audio.h"
#include "hd60s_pace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <errno.h>

// ======================================================================
// SECTION 1: parser de sincronía de frame (vídeo + SEP de audio embebido)
// ======================================================================
// parser de sincronía de frame (estructura hallada en workflow 2026-07-09)
// formato real: cada línea=3840B + marcador 4B al final; a veces se inserta SEP(0xff00ff02+12B).
// 1 frame=1080ACT+45BLK=1125 líneas. Marcadores 0xff000080=EOL_ACT (fila de vídeo), 0xff0000ab=EOL_BLK (fila de blanking), 0xff00ff02+12=SEP.
// Se pelan los marcadores y se manda 1920x1080 YUYV (4147200B) a v4l2loopback.
// ==================================================================
#define FRAME_W 1920
#define FRAME_H 1080
#define LINE_BYTES (FRAME_W * 2)                   // 3840
#define FRAME_BYTES (LINE_BYTES * FRAME_H)         // 4,147,200
#define EXPECTED_FRAME_BLK 45                      // measured 005e vertical-blanking markers/frame

// marcadores (se leen como little-endian 32bit)
#define MK_EOL_ACT 0x800000ffu   // bytes: ff 00 00 80
#define MK_EOL_BLK 0xab0000ffu   // bytes: ff 00 00 ab
#define MK_SEP     0x02ff00ffu   // bytes: ff 00 ff 02, luego saltar 12 bytes
#define MK_SEP_BULK 0x04ff00ffu  // bulk/USB3 stream uses the same SEP payload with type 04

typedef enum { HUNT = 0, LOCKED = 1 } pstate_t;
static pstate_t g_state = HUNT;
static uint8_t g_linebuf[LINE_BYTES];              // ensamblado de la línea actual
static int g_lpos = 0;                             // posición de llenado de linebuf
static uint8_t g_framebuf[FRAME_BYTES];            // frame terminado
static int g_fline = 0;                            // número de fila ACT dentro del frame
static int g_blk_run = 0;                          // contador de BLK seguidos

// pending: guarda los últimos 3 bytes + resto SEP para que el marcador 32bit no se rompa al cruzar líneas
static uint8_t g_pend[16];
static int g_pend_n = 0;

unsigned long long g_frames_out = 0;
unsigned long long g_resyncs = 0;
unsigned long long g_resync_empty = 0;   // por iso empty
unsigned long long g_resync_marker = 0;  // por marcador desconocido
unsigned long long g_resync_overflow = 0;// por work overflow
static int g_parser_synced = 0;
static uint8_t g_sync_buf[524288];
static size_t g_sync_n = 0;
static uint8_t g_dyn_buf[131072];
static size_t g_dyn_n = 0;
static uint8_t g_prev_line[LINE_BYTES];
static int g_have_prev_line = 0;
// After a transport/parser loss, do not publish the first structurally
// ambiguous 1080-line group.  Wait for a complete vertical blanking run
// before resuming output.
static int g_recovery_pending = 0;

// Opt-in first-loss tracing.  This is deliberately separate from the normal
// cadence counters: with HD60S_PARSER_TRACE unset it allocates no packet
// storage and does not change parser decisions.  When enabled, it keeps the
// last packets verbatim, records parser state around each packet, and captures
// the first parser-loss context plus the next packets used for reacquisition.
#define PARSER_TRACE_RING_DEPTH 64
#define PARSER_TRACE_MAX_PACKET (64u << 10)
#define PARSER_TRACE_AFTER_PACKETS 128
typedef struct {
    unsigned long long packet_no;
    unsigned long long callback_no;
    int packet_index;
    int transfer_status;
    int packet_status;
    unsigned int requested_length;
    unsigned int actual_length;
    int synced_before;
    int fline_before;
    int blk_before;
    size_t dyn_before;
    int synced_after;
    int fline_after;
    int blk_after;
    size_t dyn_after;
    unsigned int raw_stored;
    uint8_t head[16];
    uint8_t tail[16];
    uint8_t raw[PARSER_TRACE_MAX_PACKET];
} ParserTracePacket;

int g_trace_enabled = 0;
static int g_trace_loss_seen = 0;
static unsigned long long g_trace_packet_no = 0;
unsigned long long g_trace_callback_no = 0;
static unsigned int g_trace_after_remaining = 0;
static unsigned int g_trace_current_slot = 0;
static int g_trace_current_active = 0;
static int g_trace_post_slot = -1;
static unsigned int g_trace_ring_count = 0;
static unsigned int g_trace_ring_next = 0;
static ParserTracePacket* g_trace_packets = NULL;
static FILE* g_trace_log = NULL;
static FILE* g_trace_post_raw = NULL;
static char g_trace_base[PATH_MAX] = "/tmp/hd60s-parser-first-loss";

static unsigned long long g_trace_scan_count = 0;
static size_t g_trace_last_scan_n = 0;
static size_t g_trace_last_scan_start = 0;
static size_t g_trace_last_scan_end = 0;
static size_t g_trace_last_scan_q = SIZE_MAX;
static size_t g_trace_last_scan_next = SIZE_MAX;
static uint32_t g_trace_last_scan_tag = 0;
static unsigned int g_trace_last_scan_candidates = 0;
static unsigned int g_trace_last_scan_waiting = 0;
static unsigned int g_trace_last_scan_rejected = 0;
static unsigned int g_trace_last_scan_confirmed = 0;

static int trace_env_enabled(const char* name) {
    const char* value = getenv(name);
    return value && value[0] && value[0] != '0' && value[0] != 'n' && value[0] != 'N';
}

static void trace_hex(FILE* fp, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) fprintf(fp, "%02x", data[i]);
}

static void trace_path(char* out, size_t out_n, const char* suffix) {
    if (!out_n) return;
    size_t base_n = strlen(g_trace_base);
    size_t suffix_n = strlen(suffix);
    if (suffix_n >= out_n) suffix_n = out_n - 1;
    size_t base_room = out_n - suffix_n - 1;
    if (base_n > base_room) base_n = base_room;
    memcpy(out, g_trace_base, base_n);
    memcpy(out + base_n, suffix, suffix_n);
    out[base_n + suffix_n] = '\0';
}

static void parser_trace_init(void) {
    if (!trace_env_enabled("HD60S_PARSER_TRACE")) return;
    g_trace_packets = calloc(PARSER_TRACE_RING_DEPTH, sizeof(*g_trace_packets));
    if (!g_trace_packets) {
        fprintf(stderr, "[parser-trace] packet ring allocation failed; tracing disabled\n");
        return;
    }
    const char* base = getenv("HD60S_PARSER_TRACE_FILE");
    if (base && *base) snprintf(g_trace_base, sizeof(g_trace_base), "%s", base);
    char path[PATH_MAX];
    trace_path(path, sizeof(path), ".log");
    g_trace_log = fopen(path, "w");
    if (!g_trace_log) {
        fprintf(stderr, "[parser-trace] cannot open %s: %s; tracing disabled\n",
                path, strerror(errno));
        free(g_trace_packets);
        g_trace_packets = NULL;
        return;
    }
    setvbuf(g_trace_log, NULL, _IOLBF, 0);
    g_trace_enabled = 1;
    fprintf(stderr, "[parser-trace] enabled base=%s (first parser-loss only)\n", g_trace_base);
    fprintf(g_trace_log, "trace_version=1 base=%s\n", g_trace_base);
}

static void parser_trace_begin(unsigned long long callback_no, int packet_index,
                               int transfer_status,
                               const struct libusb_iso_packet_descriptor* d,
                               const uint8_t* data) {
    if (!g_trace_enabled || !g_trace_packets || !d) return;
    unsigned int slot = g_trace_ring_next;
    ParserTracePacket* p = &g_trace_packets[slot];
    memset(p, 0, sizeof(*p));
    p->packet_no = ++g_trace_packet_no;
    p->callback_no = callback_no;
    p->packet_index = packet_index;
    p->transfer_status = transfer_status;
    p->packet_status = d->status;
    p->requested_length = d->length;
    p->actual_length = d->actual_length;
    p->synced_before = g_parser_synced;
    p->fline_before = g_fline;
    p->blk_before = g_blk_run;
    p->dyn_before = g_dyn_n;
    if (data && d->actual_length) {
        size_t n = d->actual_length;
        size_t head_n = n < sizeof(p->head) ? n : sizeof(p->head);
        size_t tail_n = n < sizeof(p->tail) ? n : sizeof(p->tail);
        memcpy(p->head, data, head_n);
        memcpy(p->tail, data + n - tail_n, tail_n);
        p->raw_stored = n < PARSER_TRACE_MAX_PACKET ? (unsigned int)n : 0;
        if (p->raw_stored) memcpy(p->raw, data, p->raw_stored);
    }
    g_trace_current_slot = slot;
    g_trace_current_active = 1;
    g_trace_ring_next = (g_trace_ring_next + 1) % PARSER_TRACE_RING_DEPTH;
    if (g_trace_ring_count < PARSER_TRACE_RING_DEPTH) g_trace_ring_count++;

    if (g_trace_after_remaining && g_trace_post_raw && data && d->actual_length) {
        long offset = ftell(g_trace_post_raw);
        fwrite(data, 1, d->actual_length, g_trace_post_raw);
        fflush(g_trace_post_raw);
        g_trace_post_slot = (int)slot;
        fprintf(g_trace_log,
                "post-begin packet=%llu callback=%llu index=%d offset=%ld len=%u "
                "status=%d transfer=%d synced=%d ACT=%d BLK=%d dyn=%zu\n",
                p->packet_no, p->callback_no, p->packet_index, offset,
                p->actual_length, p->packet_status, p->transfer_status,
                p->synced_before, p->fline_before, p->blk_before, p->dyn_before);
        g_trace_after_remaining--;
    }
}

static void parser_trace_end(void) {
    if (!g_trace_enabled || !g_trace_packets || !g_trace_current_active) return;
    ParserTracePacket* p = &g_trace_packets[g_trace_current_slot];
    p->synced_after = g_parser_synced;
    p->fline_after = g_fline;
    p->blk_after = g_blk_run;
    p->dyn_after = g_dyn_n;
    if (g_trace_post_slot == (int)g_trace_current_slot) {
        fprintf(g_trace_log,
                "post-end packet=%llu synced=%d ACT=%d BLK=%d dyn=%zu\n",
                p->packet_no, p->synced_after, p->fline_after,
                p->blk_after, p->dyn_after);
        g_trace_post_slot = -1;
    }
    g_trace_current_active = 0;
}

static void parser_trace_dump_loss(size_t bytes_lost, int partial_lines,
                                   int partial_blks) {
    if (!g_trace_enabled || g_trace_loss_seen) return;
    g_trace_loss_seen = 1;
    char path[PATH_MAX];
    trace_path(path, sizeof(path), ".before.raw");
    FILE* before = fopen(path, "wb");
    trace_path(path, sizeof(path), ".dyn.bin");
    FILE* dyn = fopen(path, "wb");
    trace_path(path, sizeof(path), ".post.raw");
    g_trace_post_raw = fopen(path, "wb");

    fprintf(g_trace_log,
            "loss bytes_lost=%zu partial_ACT=%d partial_BLK=%d synced=%d "
            "g_fline=%d g_blk_run=%d dyn_n=%zu sync_n=%zu packet_no=%llu callback_no=%llu\n",
            bytes_lost, partial_lines, partial_blks, g_parser_synced,
            g_fline, g_blk_run, g_dyn_n, g_sync_n, g_trace_packet_no,
            g_trace_callback_no);
    fprintf(g_trace_log,
            "last_scan count=%llu dyn_n=%zu range=%zu..%zu q=%zu next=%zu "
            "tag=0x%08x candidates=%u waiting=%u rejected=%u confirmed=%u\n",
            g_trace_scan_count, g_trace_last_scan_n, g_trace_last_scan_start,
            g_trace_last_scan_end, g_trace_last_scan_q, g_trace_last_scan_next,
            g_trace_last_scan_tag, g_trace_last_scan_candidates,
            g_trace_last_scan_waiting, g_trace_last_scan_rejected,
            g_trace_last_scan_confirmed);

    if (dyn && g_dyn_n) {
        fwrite(g_dyn_buf, 1, g_dyn_n, dyn);
        fflush(dyn);
    }
    if (dyn) fclose(dyn);

    if (before && g_trace_ring_count) {
        unsigned int first = (g_trace_ring_next + PARSER_TRACE_RING_DEPTH -
                              g_trace_ring_count) % PARSER_TRACE_RING_DEPTH;
        long offset = 0;
        for (unsigned int n = 0; n < g_trace_ring_count; ++n) {
            ParserTracePacket* p = &g_trace_packets[(first + n) % PARSER_TRACE_RING_DEPTH];
            fprintf(g_trace_log,
                    "before-packet packet=%llu callback=%llu index=%d offset=%ld "
                    "len=%u requested=%u status=%d transfer=%d "
                    "before=%d/%d/%d/%zu after=%d/%d/%d/%zu raw_stored=%u head=",
                    p->packet_no, p->callback_no, p->packet_index, offset,
                    p->actual_length, p->requested_length, p->packet_status,
                    p->transfer_status, p->synced_before, p->fline_before,
                    p->blk_before, p->dyn_before, p->synced_after,
                    p->fline_after, p->blk_after, p->dyn_after,
                    p->raw_stored);
            trace_hex(g_trace_log, p->head, sizeof(p->head));
            fprintf(g_trace_log, " tail=");
            trace_hex(g_trace_log, p->tail, sizeof(p->tail));
            fprintf(g_trace_log, "\n");
            if (p->raw_stored) {
                fwrite(p->raw, 1, p->raw_stored, before);
                offset += p->raw_stored;
            }
        }
        fflush(before);
    }
    if (before) fclose(before);

    if (g_trace_current_active) {
        ParserTracePacket* p = &g_trace_packets[g_trace_current_slot];
        fprintf(g_trace_log,
                "trigger packet=%llu callback=%llu index=%d len=%u status=%d "
                "before=%d/%d/%d/%zu current=%d/%d/%d/%zu\n",
                p->packet_no, p->callback_no, p->packet_index, p->actual_length,
                p->packet_status, p->synced_before, p->fline_before,
                p->blk_before, p->dyn_before, g_parser_synced, g_fline,
                g_blk_run, g_dyn_n);
    }
    fflush(g_trace_log);
    g_trace_after_remaining = PARSER_TRACE_AFTER_PACKETS;
}

static void parser_notify_loss(size_t bytes_lost);

static void parser_reset(const char* why) {
    // se cuenta igual en cualquier state (también queremos ver resync en HUNT)
    if (strstr(why, "empty")) g_resync_empty++;
    else if (strstr(why, "marker")) g_resync_marker++;
    else if (strstr(why, "overflow")) g_resync_overflow++;
    if (g_diag) {
        g_diag_resets++;
        fprintf(stderr, "[cadence-reset] reason=%s ACT=%d BLK=%d\n", why, g_fline, g_blk_run);
    }
    g_state = HUNT; g_lpos = 0; g_fline = 0; g_blk_run = 0; g_pend_n = 0;
    g_resyncs++;
}

static void emit_frame(void) {
    uint64_t emit_start_ns = now_mono_ns();
    if (g_diag && g_diag_frame_start_ns) {
        uint64_t latency = emit_start_ns - g_diag_frame_start_ns;
        g_diag_latency_n++;
        g_diag_latency_sum += latency;
        if (latency < g_diag_latency_min) g_diag_latency_min = latency;
        if (latency > g_diag_latency_max) g_diag_latency_max = latency;
    }
    if (g_frames_out < 3) {
        unsigned long long sum = 0;
        for (size_t i = 0; i < FRAME_BYTES; i += 4096) sum += g_framebuf[i];
        fprintf(stderr, "[frame-debug] #%llu first=%02x last=%02x sample_mean=%.1f\n",
                g_frames_out + 1, g_framebuf[0], g_framebuf[FRAME_BYTES - 1],
                (double)sum / (FRAME_BYTES / 4096));
    }
    if (hd60s_v4l2_is_open() && g_pace_output) {
        hd60s_pace_push_frame(g_framebuf, g_frames_out + 1);
    } else if (hd60s_v4l2_is_open()) {
        uint64_t write_start_ns = now_mono_ns();
        ssize_t w = hd60s_v4l2_write_frame(g_framebuf, FRAME_BYTES);
        uint64_t write_done_ns = now_mono_ns();
        ++g_v4l_write_seq;
        if (g_diag)
            fprintf(stderr, "[v4l-write] seq=%llu mode=direct bytes=%zd expected=%zu %s\n",
                    g_v4l_write_seq, w, (size_t)FRAME_BYTES,
                    w == FRAME_BYTES ? "OK" : "ANOMALY");
        if (g_diag) {
            uint64_t dur = write_done_ns - write_start_ns;
            g_diag_write_dur_n++;
            g_diag_write_dur_sum += dur;
            if (dur < g_diag_write_dur_min) g_diag_write_dur_min = dur;
            if (dur > g_diag_write_dur_max) g_diag_write_dur_max = dur;
        }
        if (w == FRAME_BYTES) {
            g_diag_writes++;
            if (g_diag_last_write_ns) {
                uint64_t dt = write_done_ns - g_diag_last_write_ns;
                g_diag_interval_n++;
                g_diag_interval_sum += dt;
                if (dt < g_diag_interval_min) g_diag_interval_min = dt;
                if (dt > g_diag_interval_max) g_diag_interval_max = dt;
            }
            g_diag_last_write_ns = write_done_ns;
        } else {
            g_diag_write_fail++;
            fprintf(stderr, "[v4l2] short write %zd\n", w);
        }
    }
    g_frames_out++;
    // 2026-07-18 debug dump solo si HD60S_DEBUG_DUMP=1 (escribir 4MB × 3 todo el rato es desperdicio)
    if (hd60s_env_present("HD60S_DEBUG_DUMP") &&
        (g_frames_out == 1 || g_frames_out == 60 || g_frames_out == 300 || g_frames_out == 600)) {
        char path[256];
        snprintf(path, sizeof(path), "captures/proof/live_frame_%llu.yuv", g_frames_out);
        FILE* fp = fopen(path, "wb");
        if (fp) { fwrite(g_framebuf, 1, FRAME_BYTES, fp); fclose(fp);
                  fprintf(stderr, "[dump] %s\n", path); }
    }
    // 2026-07-18 [emit] de cada segundo → cada 5s (300 frames = 60fps × 5s).
    // bastante pronto para pillar stall, y el journal se ensucia 1/5. Equilibrio.
    // HD60S_VERBOSE=1 vuelve a cada segundo (60 frames), el verbose de antes.
    static int verbose_checked = 0, verbose = 0;
    if (!verbose_checked) { verbose = hd60s_env_on("HD60S_VERBOSE"); verbose_checked = 1; }
    unsigned long long interval = verbose ? 60 : 300;
    if ((g_frames_out % interval) == 0) fprintf(stderr, "[emit] %llu frames\n", g_frames_out);
    hd60s_diag_report_if_due();
}

// HD60 S 0fd9:005e line assembler.  Unlike the older 0074 stream, this
// revision can lose a USB block and can insert SEP records at a line edge.
// Assemble by locating the next protocol marker, padding a short line and
// discarding surplus bytes, rather than assuming a fixed marker offset.
#define MARKER_GAP_MIN 3000
#define MARKER_GAP_MAX 4100
#define AMBIGUOUS_MARKER_CONFIRM_RUN 8

static int is_protocol_marker(const uint8_t *data, size_t len, size_t pos,
                              uint32_t *tag_out) {
    if (pos > len || len - pos < 4) return 0;
    uint32_t tag;
    memcpy(&tag, data + pos, 4);
    if (tag != MK_EOL_ACT && tag != MK_EOL_BLK &&
        tag != MK_SEP && tag != MK_SEP_BULK)
        return 0;
    if (tag_out) *tag_out = tag;
    return 1;
}

// A four-byte marker can occur in YUYV pixel data.  When the normal one-line
// look-ahead rejects a candidate, look for a longer run of line-spaced
// markers before allowing the buffer to grow to the overflow limit.  This is
// only a recovery decision: an ambiguous partial frame is discarded and the
// parser reacquires from BLK->ACT, so no uncertain bytes can enter a frame.
static int find_ambiguous_marker_run(const uint8_t *data, size_t len,
                                     size_t start, size_t end,
                                     size_t *run_pos, uint32_t *run_tag) {
    if (len < 4 || start > len - 4) return 0;
    if (end > len - 4) end = len - 4;
    enum { MAX_RECOVERY_MARKERS = 512 };
    size_t positions[MAX_RECOVERY_MARKERS];
    uint32_t tags[MAX_RECOVERY_MARKERS];
    size_t marker_count = 0;
    for (size_t p = start; p <= end && marker_count < MAX_RECOVERY_MARKERS; ++p) {
        uint32_t tag;
        if (!is_protocol_marker(data, len, p, &tag)) continue;
        positions[marker_count] = p;
        tags[marker_count] = tag;
        marker_count++;
    }

    for (size_t first = 0; first < marker_count; ++first) {
        size_t cursor_index = first;
        int matched = 0;
        while (matched < AMBIGUOUS_MARKER_CONFIRM_RUN) {
            size_t next_index = cursor_index + 1;
            while (next_index < marker_count &&
                   positions[next_index] - positions[cursor_index] < MARKER_GAP_MIN)
                next_index++;
            if (next_index >= marker_count ||
                positions[next_index] - positions[cursor_index] > MARKER_GAP_MAX)
                break;
            cursor_index = next_index;
            matched++;
        }
        if (matched == AMBIGUOUS_MARKER_CONFIRM_RUN) {
            if (run_pos) *run_pos = positions[first];
            if (run_tag) *run_tag = tags[first];
            return 1;
        }
    }
    return 0;
}

static void dynamic_video_feed(const uint8_t *data, size_t len) {
    if (len > sizeof(g_dyn_buf) - g_dyn_n) {
        // Once the line buffer is full, its byte alignment is no longer
        // trustworthy.  Retaining a suffix and the old g_fline would allow
        // lines from two frames to be combined after a USB loss.
        parser_notify_loss(g_dyn_n);
        return;
    }
    size_t room = sizeof(g_dyn_buf) - g_dyn_n;
    if (len > room) len = room;
    memcpy(g_dyn_buf + g_dyn_n, data, len);
    g_dyn_n += len;

    for (;;) {
        size_t q = 0;
        uint32_t tag = 0;
        // A video line is exactly 3840 bytes followed by its marker.  Do not
        // search the whole accumulated buffer: YUYV pixels can accidentally
        // contain the marker bytes and that causes horizontal stair-stepping.
        // The marker follows one complete 1920-wide YUYV line.  Searching
        // inside the last 240 bytes of the pixel payload is unsafe: the
        // marker byte pattern can occur naturally in image data and would
        // shorten a line, permanently shifting every following line.
        // Allow only the small protocol padding window around LINE_BYTES.
        size_t scan_start = LINE_BYTES - 240;
        size_t scan_end = g_dyn_n > LINE_BYTES + 360 ? LINE_BYTES + 360 : g_dyn_n;
        size_t best_q = SIZE_MAX;
        uint32_t best_tag = 0;
        int best_score = INT_MAX;
        if (g_trace_enabled) {
            g_trace_scan_count++;
            g_trace_last_scan_n = g_dyn_n;
            g_trace_last_scan_start = scan_start;
            g_trace_last_scan_end = scan_end;
            g_trace_last_scan_q = SIZE_MAX;
            g_trace_last_scan_next = SIZE_MAX;
            g_trace_last_scan_tag = 0;
            g_trace_last_scan_candidates = 0;
            g_trace_last_scan_waiting = 0;
            g_trace_last_scan_rejected = 0;
            g_trace_last_scan_confirmed = 0;
        }
        for (size_t p = scan_start; p + 4 <= scan_end; ++p) {
            uint32_t candidate;
            memcpy(&candidate, g_dyn_buf + p, 4);
            if (candidate == MK_EOL_ACT || candidate == MK_EOL_BLK ||
                candidate == MK_SEP || candidate == MK_SEP_BULK) {
                if (g_trace_enabled) {
                    g_trace_last_scan_candidates++;
                    g_trace_last_scan_q = p;
                    g_trace_last_scan_tag = candidate;
                }
                // A marker-looking sequence in the pixel payload is not
                // sufficient.  Confirm the next protocol marker at the
                // normal line distance before accepting this candidate.
                if (g_dyn_n < p + 3004) {
                    if (g_trace_enabled) g_trace_last_scan_waiting++;
                    break;
                }
                int next_ok = 0;
                size_t next_pos = 0;
                for (size_t s = p + 3000; s <= p + 4100 && s + 4 <= g_dyn_n; ++s) {
                    uint32_t next;
                    memcpy(&next, g_dyn_buf + s, 4);
                    if (next == MK_EOL_ACT || next == MK_EOL_BLK ||
                        next == MK_SEP || next == MK_SEP_BULK) {
                        next_ok = 1;
                        next_pos = s;
                        break;
                    }
                }
                if (next_ok) {
                    if (g_trace_enabled) {
                        g_trace_last_scan_confirmed++;
                        g_trace_last_scan_next = next_pos;
                    }
                    int score = abs((int)p - (LINE_BYTES + 6)) +
                                abs((int)(next_pos - p) - (LINE_BYTES + 8));
                    if (score < best_score) {
                        best_score = score;
                        best_q = p;
                        best_tag = candidate;
                    }
                } else if (g_trace_enabled) {
                    g_trace_last_scan_rejected++;
                }
            }
        }
        if (best_q != SIZE_MAX) { q = best_q; tag = best_tag; }
        if (!q) {
            // If a candidate in the first-line window was rejected, do not
            // wait for the 64 KiB overflow guard.  A later run of markers at
            // line cadence proves that the current partial image is
            // ambiguous; discard it immediately and reacquire at the next
            // vertical BLK->ACT transition.  This specifically handles a
            // pixel false positive such as the repeatable 2732-byte gap seen
            // in the first parser-loss trace.
            if (g_dyn_n > LINE_BYTES + MARKER_GAP_MAX) {
                size_t run_pos = SIZE_MAX;
                uint32_t run_tag = 0;
                size_t search_end = g_dyn_n < 65536 ? g_dyn_n : 65536;
                if (find_ambiguous_marker_run(g_dyn_buf, g_dyn_n,
                                               scan_start, search_end,
                                               &run_pos, &run_tag)) {
                    fprintf(stderr,
                            "[parser-loss] ambiguous marker run at offset=%zu "
                            "tag=0x%08x; reacquiring at next BLK->ACT\n",
                            run_pos, run_tag);
                    parser_notify_loss(g_dyn_n);
                    return;
                }
            }
            // A vertical blanking interval can be much larger than the
            // normal 4/16-byte line padding.  If the buffer grows without a
            // confirmed marker, the byte alignment is lost; discard the
            // partial image and let parser_feed reacquire BLK->ACT.
            if (g_dyn_n > 65536) {
                // Do not lock on an ACT run in the middle of a frame.  A
                // transport gap invalidates the current image; wait for a
                // fresh BLK->ACT transition in parser_feed instead.
                parser_notify_loss(g_dyn_n);
                return;
            }
            break;
        }

        if (g_diag) {
            g_diag_markers++;
            if (q < g_diag_q_min) g_diag_q_min = q;
            if (q > g_diag_q_max) g_diag_q_max = q;
            if (q < 3800 || q > 3900) g_diag_q_bad++;
        }

        memset(g_linebuf, 0, LINE_BYTES);
        size_t copy = q < LINE_BYTES ? q : LINE_BYTES;
        memcpy(g_linebuf, g_dyn_buf, copy);
        if (copy < LINE_BYTES && g_have_prev_line)
            memcpy(g_linebuf + copy, g_prev_line + copy, LINE_BYTES - copy);
        // SEP va pegado al marcador de línea: [SEP 4B][PCM 8B][EOL_ACT|EOL_BLK].
        // El scorer suele elegir el EOL, así que el audio de blanking (BLK,
        // ~4 % del tiempo) se perdía y PipeWire rellenaba huecos → audio sucio.
        if ((tag == MK_EOL_ACT || tag == MK_EOL_BLK) && q >= 12) {
            uint32_t sep_tag;
            memcpy(&sep_tag, g_dyn_buf + q - 12, 4);
            if (sep_tag == MK_SEP || sep_tag == MK_SEP_BULK) {
                hd60s_audio_feed_sep(g_dyn_buf + q - 8);
            } else if (q >= 16) {
                memcpy(&sep_tag, g_dyn_buf + q - 16, 4);
                if (sep_tag == MK_SEP || sep_tag == MK_SEP_BULK)
                    hd60s_audio_feed_sep(g_dyn_buf + q - 12);
            }
        }
        if (tag == MK_EOL_ACT || tag == MK_SEP || tag == MK_SEP_BULK) {
            if (g_diag) g_diag_act++;
            if (g_diag && g_fline == 0 && !g_diag_frame_start_ns)
                g_diag_frame_start_ns = now_mono_ns();
            memcpy(g_prev_line, g_linebuf, LINE_BYTES);
            g_have_prev_line = 1;
            if (g_fline < FRAME_H)
                memcpy(g_framebuf + (size_t)g_fline * LINE_BYTES, g_linebuf, LINE_BYTES);
            g_fline++;
            if (g_fline >= FRAME_H) {
                int frame_lines = g_fline;
                int frame_blks = g_blk_run;
                int structurally_valid = frame_blks == EXPECTED_FRAME_BLK;
                if (structurally_valid) {
                    if (g_diag)
                        fprintf(stderr, "[frame] emit #%llu lines=%d BLK=%d %s\n",
                                g_frames_out + 1, frame_lines, frame_blks,
                                frame_blks == EXPECTED_FRAME_BLK ? "OK" : "ANOMALY");
                    if (g_diag) g_diag_complete++;
                    emit_frame();
                    g_recovery_pending = 0;
                } else {
                    fprintf(stderr,
                            "[frame-drop] complete lines=%d BLK=%d expected=%d reason=structural\n",
                            frame_lines, frame_blks, EXPECTED_FRAME_BLK);
                    if (g_diag) g_diag_discarded++;
                    // A wrong BLK count means the vertical phase is not
                    // trustworthy even when no packet error was reported.
                    // Require the next complete 45-BLK group before output.
                    g_recovery_pending = 1;
                }
                g_fline = 0;
                g_blk_run = 0;
                g_diag_frame_start_ns = 0;
            }
        } else if (tag == MK_EOL_BLK) {
            // BLK is retained as a structural diagnostic.  It is not the
            // frame delimiter: the 005e stream normally has about 29 BLK
            // markers around each group of 1080 video-line events.
            g_blk_run++;
            if (g_diag) g_diag_blk++;
        }

        size_t consume = q + 4;
        if (tag == MK_SEP || tag == MK_SEP_BULK) {
            if (g_dyn_n < q + 16) break;
            hd60s_audio_feed_sep(g_dyn_buf + q + 4);
            consume = q + 16; // SEP + 8-byte audio payload + following EOL
        }
        if (consume > g_dyn_n) break;
        memmove(g_dyn_buf, g_dyn_buf + consume, g_dyn_n - consume);
        g_dyn_n -= consume;
        hd60s_diag_report_if_due();
    }
}

// consume data/len y se lo mete al parser. iso packet máx 32768B, pending máx 16B, así que
// work se dimensiona con holgura de sobra.
static void parser_feed(const uint8_t* data, size_t len) {
    if (!g_parser_synced) {
        size_t take = len;
        size_t room = sizeof(g_sync_buf) - g_sync_n;
        if (take > room) {
            // The device can send a long pre-video/control interval before
            // the first stable ACT run.  Keep this as a sliding window;
            // otherwise the original 512 KiB would fill once and prevent
            // the real video start from ever being examined.
            if (take >= sizeof(g_sync_buf)) {
                data += len - sizeof(g_sync_buf);
                take = sizeof(g_sync_buf);
                g_sync_n = 0;
            } else {
                size_t drop = take - room;
                memmove(g_sync_buf, g_sync_buf + drop, g_sync_n - drop);
                g_sync_n -= drop;
            }
        }
        memcpy(g_sync_buf + g_sync_n, data, take);
        g_sync_n += take;
        // Do not lock on an arbitrary ACT run.  The video phase begins with
        // a vertical blanking run (normally 45 BLK markers) followed by the
        // first ACT line.  The stream can contain a long ACT preamble before
        // that BLK run; locking on eight ACT markers there starts the frame at
        // an arbitrary vertical line and creates a continuous roll.  Require
        // a real BLK->ACT transition and start immediately after the final
        // BLK marker, so the next 1080 ACT events are one complete frame.
        const int LOCK_BLK = 30;
        const int LOCK_ACT = 8;
        for (size_t p = 0; p + 3004 * (LOCK_BLK + LOCK_ACT) <= g_sync_n; ++p) {
            uint32_t first;
            memcpy(&first, g_sync_buf + p, 4);
            if (first != MK_EOL_BLK) continue;

            size_t last_blk = p;
            int blk_count = 1;
            while (blk_count < 60) {
                size_t lo = last_blk + 3000;
                size_t hi = last_blk + 4100;
                if (hi + 4 > g_sync_n) break;
                size_t next_pos = SIZE_MAX;
                uint32_t next = 0;
                for (size_t q = lo; q <= hi; ++q) {
                    memcpy(&next, g_sync_buf + q, 4);
                    if (next == MK_EOL_BLK || next == MK_EOL_ACT) {
                        next_pos = q;
                        break;
                    }
                }
                if (next_pos == SIZE_MAX || next != MK_EOL_BLK) break;
                last_blk = next_pos;
                blk_count++;
            }
            if (blk_count < LOCK_BLK) continue;

            size_t cursor = last_blk;
            int matched = 1;
            for (int k = 0; k < LOCK_ACT; ++k) {
                size_t lo = cursor + 3000;
                size_t hi = cursor + 4100;
                if (hi + 4 > g_sync_n) { matched = 0; break; }
                int found = 0;
                for (size_t q = lo; q <= hi; ++q) {
                    uint32_t next;
                    memcpy(&next, g_sync_buf + q, 4);
                    if (next == MK_EOL_ACT) {
                        cursor = q;
                        found = 1;
                        break;
                    }
                }
                if (!found) { matched = 0; break; }
            }
            if (matched) {
                g_parser_synced = 1;
                size_t off = last_blk + 4;
                size_t remain = g_sync_n - off;
                while (remain) {
                    size_t chunk = remain > 60000 ? 60000 : remain;
                    dynamic_video_feed(g_sync_buf + off, chunk);
                    off += chunk;
                    remain -= chunk;
                }
                g_sync_n = 0;
                data = NULL;
                len = 0;
                break;
            }
        }
        if (!g_parser_synced) return;
    }
    dynamic_video_feed(data, len);
    return;
    static uint8_t work[65536 + 64];
    if (g_pend_n + len > sizeof(work)) {
        // paquete enorme inesperado. recortar y resync
        parser_reset("work overflow");
        return;
    }
    memcpy(work, g_pend, g_pend_n);
    memcpy(work + g_pend_n, data, len);
    size_t total = g_pend_n + len;
    g_pend_n = 0;
    size_t i = 0;

    while (i < total) {
        // llenar linebuf con 3840B (g_lpos puede venir a medias de la vez anterior)
        size_t need = LINE_BYTES - g_lpos;
        if (need > 0) {
            size_t avail = total - i;
            size_t take = (avail < need) ? avail : need;
            memcpy(g_linebuf + g_lpos, work + i, take);
            g_lpos += take; i += take;
            if (g_lpos < LINE_BYTES) break;  // la línea no está llena = el resto se lleva a la próxima
        }

        // para leer el marcador hacen falta 4B (16B si SEP). Si no alcanzan, a pending.
        // ★★★IMPORTANTE★★★ al dejar resto en pending, g_lpos se queda en LINE_BYTES.
        // (en el siguiente parser_feed g_lpos==LINE_BYTES, need==0, se salta el relleno
        //  y se pasa directo a juzgar el marcador)
        if (total - i < 4) {
            size_t r = total - i;
            memcpy(g_pend, work + i, r); g_pend_n = r;
            return;
        }
        uint32_t tag; memcpy(&tag, work + i, 4);
        // This HD60 S revision occasionally inserts four or twelve bytes
        // around a line boundary.  Locate the next protocol marker instead
        // of assuming it is exactly at byte 3840.
        size_t marker_i = i;
        size_t marker_end = total < i + 64 ? total : i + 64;
        for (size_t q = i; q + 4 <= marker_end; ++q) {
            uint32_t candidate;
            memcpy(&candidate, work + q, 4);
            if (candidate == MK_EOL_ACT || candidate == MK_EOL_BLK ||
                candidate == MK_SEP || candidate == MK_SEP_BULK) {
                marker_i = q;
                tag = candidate;
                break;
            }
        }
        i = marker_i;

        // SEP: 4B magic + 8B payload + 4B marcador (EOL_ACT etc.) = 16B en total.
        // medido: en SEP+12 siempre sigue EOL_ACT (ff 00 00 80) (201/201=100%).
        // → la impl: «si ves SEP magic, avanza +12 y relee el marcador 4B de justo después».
        //   los 8B del SEP payload pueden ser audio (el parser los salta; otro parser de audio los trata).
        if (tag == MK_SEP || tag == MK_SEP_BULK) {
            if (total - i < 16) {
                size_t r = total - i;
                memcpy(g_pend, work + i, r); g_pend_n = r;
                return;
            }
            // payload = 8 bytes, posiciones i+4 .. i+11 (audio: 48kHz s16le stereo, 2 frames/SEP)
            hd60s_audio_feed_sep(work + i + 4);
            i += 12;                          // saltar hasta el payload 8B (magic 4B + 8B payload)
            memcpy(&tag, work + i, 4);        // releer el marcador 4B
        }
        i += 4;

        if (tag == MK_EOL_ACT) {
            if (g_state != LOCKED) {
                if (g_blk_run >= 30) { g_state = LOCKED; g_fline = 0; }
                else { g_lpos = 0; continue; }
            }
            if (g_blk_run > 0 && g_fline >= FRAME_H) {
                emit_frame();
                g_fline = 0;
            }
            g_blk_run = 0;
            if (g_fline < FRAME_H) memcpy(g_framebuf + g_fline * LINE_BYTES, g_linebuf, LINE_BYTES);
            g_fline++;
            g_lpos = 0;
        } else if (tag == MK_EOL_BLK) {
            g_blk_run++;
            g_lpos = 0;
        } else {
            // marcador desconocido: no reset; retrocede 4B y avanza 1B para rebuscar. El marcador de verdad debería estar en 3844B.
            // así se evita falso sync si un byte de vídeo coincide por casualidad con un marcador.
            i -= 3;
            // desplazar ese byte de linebuf y tratarlo como «línea que empieza 1B antes» es complicado, así que
            // en LOCKED se tira la línea (g_lpos=0) y se busca el siguiente marcador de verdad.
            // control de frecuencia: para no resetear de más, solo se incrementa el contador
            g_resync_marker++;
            g_lpos = 0;
        }
    }
}

// A packet-level USB error means that bytes in the current line/frame are
// missing.  Discard the partial frame and require a fresh BLK->ACT lock; if
// we kept g_fline, subsequent lines could be combined with the old frame and
// produce a vertically rolling image.  A completed iso packet with
// actual_length==0 is deliberately not routed here: on this device it can be
// a normal no-data interval during blanking.
static void parser_notify_loss(size_t bytes_lost) {
    (void)bytes_lost;
    int partial_lines = g_fline;
    parser_trace_dump_loss(bytes_lost, partial_lines, g_blk_run);
    if (g_diag) {
        if (partial_lines > 0) g_diag_partial++;
        g_diag_resets++;
        fprintf(stderr, "[parser-loss] discarded partial frame ACT=%d BLK=%d\n",
                g_fline, g_blk_run);
    }
    // The dynamic parser owns the active line assembly; resetting only the
    // legacy g_lpos/g_pend state is insufficient and would leave stale bytes
    // in g_dyn_buf.
    g_lpos = 0;
    g_pend_n = 0;
    g_dyn_n = 0;
    g_sync_n = 0;
    g_parser_synced = 0;
    g_fline = 0;
    g_blk_run = 0;
    g_recovery_pending = 1;
    g_have_prev_line = 0;
    g_diag_frame_start_ns = 0;
    g_resyncs++;
}

void hd60s_parser_feed(const uint8_t *data, size_t len) {
    parser_feed(data, len);
}

void hd60s_parser_notify_loss(size_t bytes_lost) {
    parser_notify_loss(bytes_lost);
}

void hd60s_parser_reset(const char *why) {
    parser_reset(why);
}

void hd60s_parser_reset_video_phase(void) {
    g_parser_synced = 0;
    g_sync_n = 0;
    g_dyn_n = 0;
    g_fline = 0;
    g_blk_run = 0;
    g_lpos = 0;
    g_have_prev_line = 0;
}

void hd60s_parser_trace_init(void) {
    parser_trace_init();
}

void hd60s_parser_trace_begin(unsigned long long callback_no, int packet_index,
                              int transfer_status,
                              const struct libusb_iso_packet_descriptor *d,
                              const uint8_t *data) {
    parser_trace_begin(callback_no, packet_index, transfer_status, d, data);
}

void hd60s_parser_trace_end(void) {
    parser_trace_end();
}
