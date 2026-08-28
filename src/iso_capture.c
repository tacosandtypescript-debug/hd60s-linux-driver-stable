// HD60 S: 呪文再生 + iso(alt2) キャプチャ (libusb-1.0, C)
// node-usb が iso 非対応なので C で実装。Windows と同じ iso 経路で EP0x83 から映像(生YUYV想定)を吸う。
//
// build: gcc -O2 -Isrc src/iso_capture.c src/hd60s_util.c src/hd60s_v4l2.c src/hd60s_audio.c src/hd60s_replay.c src/hd60s_pace.c src/hd60s_parser.c -o iso_capture $(pkg-config --libs --cflags libusb-1.0)
// run  : sudo ./iso_capture [readSec=6] [alt=2] > /dev/null  (映像は captures/stream-iso.bin へ)
//
// ======================================================================
// TODO (未解決の残作業):
//   - MCU 経由の音声パス確立: 現状 SEP payload 8B から音声を抜いているが、
//     ソースによっては無音のまま。IT6802E/IT66121 の unmute 手順 (HD60S_UNMUTE)
//     と 0x509c MCU コマンドの組み合わせが未確定。
//   - パススルー (HDMI OUT へのループスルー): pt-only モードで一時的に
//     出力するが、IT66121 の AV_MUTE/SW_RST/AFE の恒久的解放シーケンスが
//     ホスト側/MCU側どちらの責務か未特定。keepalive-cycle を回避する形の
//     "投げっぱなし" 経路を探索中。
// ======================================================================
//
// ======================================================================
// SECTION 0: ヘッダ / 定数 / グローバル状態
// ======================================================================
#include <libusb-1.0/libusb.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <samplerate.h>
#include <linux/videodev2.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sched.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include "hd60s_util.h"
#include "hd60s_v4l2.h"
#include "hd60s_audio.h"
#include "hd60s_replay.h"
#include "hd60s_pace.h"
#include "hd60s_parser.h"

#define MAX_WRITE_BYTES (64LL << 20)  // 出力ファイルは先頭64MBまで(2Gbpsで肥大化防止)

#define VID 0x0fd9              // Elgato USB vendor ID
#define PID 0x005e              // Original HD60 S detected on this PC
#define EP_STREAM 0x83          // iso IN endpoint (映像+SEP embedded audio)
// マジックナンバーの意味:
//   wValue = 0x5066  → I2C ブリッジ経路 (bank+reg 指定で IT6802E/IT66121 を叩く)
//   wValue = 0x509c  → MCU コマンド経路 (Cypress FX3 内の MCU 相当への 2B コマンド)
//   bRequest = 0xC0/0xC6 → ベンダ固有 (詳細は notes/protocol.md 参照予定)
//   bank 0x9a=IT66121 write, 0x9b=IT66121 read setup, 0x9c=IT6802E write, 0x9d=IT6802E read setup
    // SuperSpeed iso: each libusb iso descriptor represents one service
    // interval payload for this device.  The SS companion descriptors report:
    // Alt2: wMaxPacketSize=1024, bMaxBurst=15, Mult=1 -> 32768 B/interval.
    // Alt3: wMaxPacketSize=1024, bMaxBurst=12, Mult=2 -> 39936 B/interval.
    // The multiplier is zero-based in the descriptor.
// Windows実測: 32 iso pkt/URB × 16 in-flight = 16MB/64ms でempty 0.07%。
// Linuxではさらにキュー深度を増やして URB 完了→再サブミット遅延の吸収余裕を確保。
// libuvcのLinuxデフォルト=100本。8-64本試行で empty% と CPU 負荷のバランスを取る。
#define NUM_TRANSFERS 506     // tested xHCI ceiling: 512 attempted, 506 accepted

static FILE* outf;
static long long total_bytes = 0;
static long pkt_ok = 0, pkt_empty = 0, pkt_err = 0;
static int keep_running = 1;
static int inflight = 0;
static int max_inflight = 0;
static unsigned long submit_ok = 0, submit_fail = 0, resubmit_fail = 0;
static int usb_session_fatal = 0;
static int usb_session_error = 0;
// Return the payload capacity advertised for EP_STREAM in the selected
// alternate setting.  On SuperSpeed devices the companion descriptor's
// wBytesPerInterval is the authoritative service-interval capacity; for
// high-speed descriptors, derive it from wMaxPacketSize and its transaction
// multiplier.  Keeping this check next to URB construction prevents a
// total-interval size from being silently used as an oversized packet.
static int iso_endpoint_capacity(libusb_device_handle* h, int alt,
                                 int* capacity, int* superspeed) {
    struct libusb_config_descriptor* cfg = NULL;
    libusb_device* dev = libusb_get_device(h);
    int rc = libusb_get_active_config_descriptor(dev, &cfg);
    if (rc < 0) rc = libusb_get_config_descriptor(dev, 0, &cfg);
    if (rc < 0 || !cfg) return rc < 0 ? rc : LIBUSB_ERROR_OTHER;

    int found = LIBUSB_ERROR_NOT_FOUND;
    *capacity = 0;
    *superspeed = 0;
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        const struct libusb_interface* iface = &cfg->interface[i];
        for (int a = 0; a < iface->num_altsetting; a++) {
            const struct libusb_interface_descriptor* setting = &iface->altsetting[a];
            if (setting->bInterfaceNumber != 0 || setting->bAlternateSetting != alt)
                continue;
            for (int e = 0; e < setting->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor* ep = &setting->endpoint[e];
                if (ep->bEndpointAddress != EP_STREAM ||
                    (ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS)
                    continue;

                int max_packet = ep->wMaxPacketSize & 0x07ff;
                int transactions = ((ep->wMaxPacketSize >> 11) & 3) + 1;
                struct libusb_ss_endpoint_companion_descriptor* ss = NULL;
                int ss_rc = libusb_get_ss_endpoint_companion_descriptor(NULL, ep, &ss);
                if (ss_rc == 0 && ss) {
                    *superspeed = 1;
                    *capacity = ss->wBytesPerInterval;
                    fprintf(stderr,
                            "[usb] alt=%d ep=0x%02x wMax=0x%04x SS burst=%u mult=%u bytes/interval=%u\n",
                            alt, ep->bEndpointAddress, ep->wMaxPacketSize,
                            ss->bMaxBurst, ss->bmAttributes & 3,
                            ss->wBytesPerInterval);
                    libusb_free_ss_endpoint_companion_descriptor(ss);
                } else {
                    *capacity = max_packet * transactions;
                    fprintf(stderr,
                            "[usb] alt=%d ep=0x%02x wMax=0x%04x max_packet=%d transactions=%d bytes/interval=%d\n",
                            alt, ep->bEndpointAddress, ep->wMaxPacketSize,
                            max_packet, transactions, *capacity);
                }
                found = (*capacity > 0) ? 0 : LIBUSB_ERROR_INVALID_PARAM;
                break;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return found;
}


// ======================================================================
// SECTION 3: USB 送受信 (呪文再生 / iso コールバック / burst 発火)
// ======================================================================
static void LIBUSB_CALL iso_cb(struct libusb_transfer* xfr) {
    static unsigned long callback_count = 0;
    unsigned long long trace_cb_no = 0;
    if (g_trace_enabled) trace_cb_no = ++g_trace_callback_no;
    // HEX DUMP HOOK: HD60S_HEXDUMP=1 で iso packet の先頭 32B を最初 500 個 dump
    // 音声パケットと映像パケットを見分けるため (workflow suggestion 2026-07-11)
    static const char* env_hexdump = NULL;
    static int hexdump_check = 0;
    static int hexdump_count = 0;
    if (!hexdump_check) {
        env_hexdump = getenv("HD60S_HEXDUMP");
        hexdump_check = 1;
    }
    int do_hexdump = env_hexdump && env_hexdump[0] && env_hexdump[0] != '0';

    int packet_loss = 0;
    size_t lost_bytes = 0;
    for (int i = 0; i < xfr->num_iso_packets; i++) {
        struct libusb_iso_packet_descriptor* d = &xfr->iso_packet_desc[i];
        unsigned char* packet_buf = NULL;
        if (d->status == LIBUSB_TRANSFER_COMPLETED && d->actual_length > 0)
            packet_buf = libusb_get_iso_packet_buffer_simple(xfr, i);
        hd60s_parser_trace_begin(trace_cb_no, i, xfr->status, d, packet_buf);
        if (d->status == LIBUSB_TRANSFER_COMPLETED) {
            if (d->actual_length > 0) {
                unsigned char* buf = packet_buf;
                // HEX DUMP hook - first 500 non-empty packets
                if (do_hexdump && hexdump_count < 500) {
                    fprintf(stderr, "PKT[%d] len=%d: ", hexdump_count, d->actual_length);
                    for (unsigned int b = 0; b < 32 && b < d->actual_length; b++) fprintf(stderr, "%02x", buf[b]);
                    fprintf(stderr, "\n");
                    hexdump_count++;
                }
                // parser にライブ供給 (v4l2loopback へ流す)
                hd60s_parser_feed(buf, d->actual_length);
                // 検証用: 先頭512MBだけ生ストリームを保存 (音声解析用に増量)
                if (total_bytes < (512LL << 20) && outf) fwrite(buf, 1, d->actual_length, outf);
                total_bytes += d->actual_length;
                if (g_diag) g_diag_input_bytes += d->actual_length;
                pkt_ok++;
                hd60s_parser_trace_end();
            } else {
                // iso 0-length pkt はブランキングによる正常な休止と考え、
                // 進行中のライン位置には触れない (実測で empty時にg_lpos リセットすると
                // 逆に marker resync が増える → 触らないのが正解)。
                pkt_empty++;
                hd60s_parser_trace_end();
            }
        } else {
            pkt_err++;
            packet_loss = 1;
            lost_bytes += d->length;
            if (xfr->status == LIBUSB_TRANSFER_NO_DEVICE ||
                xfr->status == LIBUSB_TRANSFER_ERROR) {
                usb_session_fatal = 1;
                usb_session_error = xfr->status;
            }
            hd60s_parser_trace_end();
        }
    }
    if (packet_loss)
        hd60s_parser_notify_loss(lost_bytes);
    if (!g_stop_requested && keep_running && !usb_session_fatal) {
        int submit_rc = libusb_submit_transfer(xfr);
        if (submit_rc < 0) {
            inflight--;
            resubmit_fail++;
            usb_session_fatal = 1;
            usb_session_error = submit_rc;
            keep_running = 0;
            fprintf(stderr, "[iso] resubmit failed rc=%d (%s); ending USB session\n",
                    submit_rc, libusb_error_name(submit_rc));
        }
    } else {
        inflight--;
    }
    if ((++callback_count % 100) == 0)
        fprintf(stderr, "[iso-debug] callbacks=%lu ok=%ld empty=%ld err=%ld bytes=%lld frames=%llu\\n",
                callback_count, pkt_ok, pkt_empty, pkt_err, total_bytes, g_frames_out);
}

// Stop every outstanding transfer before the device handle is released.  A
// completed callback may resubmit the same transfer, so setting keep_running
// alone is not enough: all still-submitted URBs must be cancelled and their
// cancellation callbacks drained first.
static void cleanup_iso_transfers(libusb_device_handle* h,
                                  struct libusb_transfer** xfrs,
                                  unsigned char** bufs,
                                  const unsigned char* devmem,
                                  int count, size_t buffer_bytes) {
    if (!xfrs || !bufs) return;
    keep_running = 0;

    for (int i = 0; i < count; i++) {
        if (!xfrs[i]) continue;
        int rc = libusb_cancel_transfer(xfrs[i]);
        if (rc < 0 && rc != LIBUSB_ERROR_NOT_FOUND && rc != LIBUSB_ERROR_NO_DEVICE)
            fprintf(stderr, "[iso] cancel%d failed rc=%d (%s)\n",
                    i, rc, libusb_error_name(rc));
    }

    // Cancellation is asynchronous.  Let libusb deliver every callback so
    // the inflight count reaches zero before transfer objects are freed.
    struct timeval tv = {0, 20000};
    for (int wait = 0; wait < 100 && inflight > 0; wait++) {
        int rc = libusb_handle_events_timeout(NULL, &tv);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED && rc != LIBUSB_ERROR_NO_DEVICE)
            fprintf(stderr, "[iso] cancel-drain failed rc=%d (%s)\n",
                    rc, libusb_error_name(rc));
    }
    if (inflight > 0)
        fprintf(stderr, "[iso] warning: %d transfers remained after cancellation drain\n",
                inflight);
    if (inflight > 0) {
        // Do not free a transfer object that libusb still considers active.
        // The process is exiting and the handle teardown will reclaim the
        // remaining OS resources; freeing here would be use-after-free if a
        // late cancellation callback is delivered.
        return;
    }

    for (int i = 0; i < count; i++) {
        if (xfrs[i]) {
            libusb_free_transfer(xfrs[i]);
            xfrs[i] = NULL;
        }
        if (!bufs[i]) continue;
        if (devmem && devmem[i]) libusb_dev_mem_free(h, bufs[i], buffer_bytes);
        else free(bufs[i]);
        bufs[i] = NULL;
    }
}

// ======================================================================
// SECTION 4: main (デバイス open → 呪文再生 → iso キャプチャ → 統計出力)
// ======================================================================
// TODO: パススルー (HDMI ループスルー) の恒久有効化と、それを維持したまま
// キャプチャする経路の両立が未解決。現状は pt-only モードで一時的に維持のみ。
int main(int argc, char** argv) {
    hd60s_diag_set(getenv("HD60S_CADENCE_DIAG") ? 1 : 0);
    const char *pace_env = getenv("HD60S_PACE_OUTPUT");
    hd60s_pace_configure(pace_env && pace_env[0] && pace_env[0] != '0' &&
                    pace_env[0] != 'n' && pace_env[0] != 'N');
    if (g_diag) fprintf(stderr, "[cadence] diagnostics enabled (no capture parameters changed)\n");
    if (g_pace_output) fprintf(stderr, "[cadence] V4L2 output pacing enabled at 60 Hz\n");
    // stderr を無バッファに (SCHED_FIFO でリアルタイム優先度にすると
    // ブロックバッファになってログが即表示されない問題の対策)
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    hd60s_parser_trace_init();
    install_signal_handlers();
    int read_sec = argc > 1 ? atoi(argv[1]) : 6;
    // 秒数 0 or 負数を「実用上無限 (~68 年)」に扱う
    // 注: 100 年 = 3,153,600,000 は int overflow なので INT_MAX-3600 で安全
    if (read_sec <= 0) read_sec = 2147480047;  // INT_MAX - 3600, ~68 years
    // The default is Alt2, confirmed from the device descriptor as
    // bMaxBurst=15, Mult=1, with 32768 bytes per service interval.
    int alt = argc > 2 ? atoi(argv[2]) : 2;
    // On this device/libusb stack, one iso descriptor accepts the complete
    // Alt2 service-interval payload.  32768 x 1 was verified on hardware
    // without EMSGSIZE and restored the expected USB throughput.
    int iso_pkt_size = (alt == 2) ? 32768 : 1024;
    const char *pkt_env = getenv("HD60S_ISO_PKT");
    if (pkt_env && *pkt_env) iso_pkt_size = atoi(pkt_env);
    if (iso_pkt_size < 1024) iso_pkt_size = 1024;
    // One descriptor corresponds to one complete service interval.  Queue
    // depth is provided by NUM_TRANSFERS, not by subdividing the burst.
    int iso_packets = (alt == 2) ? 1 : 32;
    const char *packets_env = getenv("HD60S_ISO_PKTS");
    if (packets_env && *packets_env) iso_packets = atoi(packets_env);
    if (iso_packets < 1) iso_packets = 1;
    fprintf(stderr, "[iso] requested packet length=%d\n", iso_pkt_size);
    // 5番目の引数が "pt" ならパススルー専用モード（iso張らず、9a burstだけ撃って維持）
    int passthrough_only = (argc > 5 && strcmp(argv[5], "pt") == 0);
    // ラベルは 5番目 (pt モードでは 6番目)
    if (passthrough_only && argc > 6) argv[4] = argv[6];

    // 2026-07-10 SCHED_FIFO を撤去: カクツキ改善効果が実測ゼロだった一方、
    // このプロセスがCPUを独占しlibusbの内部処理(別スレッド/カーネルワーカー)が
    // スケジューリングされず replay_spell が "open/claim OK" 以降ハングする
    // 重大な副作用が判明(再現確認済み, CPU 0%で停止)。mlockallのみ残す(害なし)。
    if (mlockall(MCL_CURRENT | MCL_FUTURE) < 0) fprintf(stderr, "[main] mlockall 失敗: %s(続行)\n", strerror(errno));

    int libusb_rc = libusb_init(NULL);
    if (libusb_rc < 0) {
        fprintf(stderr, "libusb_init failed rc=%d (%s)\n", libusb_rc,
                libusb_error_name(libusb_rc));
        return 2;
    }
    libusb_device_handle* h = libusb_open_device_with_vid_pid(NULL, VID, PID);
    if (!h) {
        fprintf(stderr, "デバイスopen失敗 (0fd9:005e)\n");
        libusb_exit(NULL);
        return 2;
    }
    libusb_set_auto_detach_kernel_driver(h, 1);
    // 2026-07-10 デバイス強制リセット: 前回異常終了後や物理抜き差し後にI2Cバスがロックしたり
    // 内部状態が乱れる問題への対策 (実測: reset無しだと 9d:0x12 が0x9d返しで100%空パケット)
    // HD60S_NO_RESET=1 で skip (passthrough 状態を壊したくない時用)
    const char* env_no_reset = getenv("HD60S_NO_RESET");
    int no_reset = (env_no_reset && env_no_reset[0] && env_no_reset[0] != '0' && env_no_reset[0] != 'n' && env_no_reset[0] != 'N');
    int rst = no_reset ? 0 : libusb_reset_device(h);
    if (no_reset) fprintf(stderr, "[main] reset 省略 (HD60S_NO_RESET)\n");
    if (rst == LIBUSB_ERROR_NOT_FOUND) {
        // reset後にデバイスIDが変わることがある→再オープン
        fprintf(stderr, "[main] reset後デバイス再列挙、再オープン中...\n");
        libusb_close(h);
        int retry = 0;
        while (retry++ < 20) {
            usleep(200000);
            h = libusb_open_device_with_vid_pid(NULL, VID, PID);
            if (h) break;
        }
        if (!h) {
            fprintf(stderr, "reset後デバイス再open失敗\n");
            libusb_exit(NULL);
            return 2;
        }
        libusb_set_auto_detach_kernel_driver(h, 1);
    } else if (rst < 0) {
        fprintf(stderr, "[main] reset failed rc=%d (%s); aborting USB session\n",
                rst, libusb_error_name(rst));
        libusb_close(h);
        libusb_exit(NULL);
        return 2;
    } else {
        fprintf(stderr, "[main] リセット成功\n");
    }
    int cfg_rc = libusb_set_configuration(h, 1);
    if (cfg_rc < 0 && cfg_rc != LIBUSB_ERROR_BUSY) {
        fprintf(stderr, "set_config failed rc=%d (%s)\n", cfg_rc, libusb_error_name(cfg_rc));
        libusb_close(h);
        libusb_exit(NULL);
        return 2;
    }
    int claim_rc = libusb_claim_interface(h, 0);
    if (claim_rc < 0) {
        fprintf(stderr, "claim failed rc=%d (%s)\n", claim_rc, libusb_error_name(claim_rc));
        libusb_close(h);
        libusb_exit(NULL);
        return 2;
    }
    fprintf(stderr, "[main] open/claim OK (リセット後)\n");

    if (alt != 4) {
        int endpoint_capacity = 0;
        int endpoint_superspeed = 0;
        int endpoint_rc = iso_endpoint_capacity(h, alt, &endpoint_capacity,
                                                 &endpoint_superspeed);
        if (endpoint_rc < 0 || endpoint_capacity <= 0) {
            fprintf(stderr, "[usb] no usable isochronous endpoint for alt=%d rc=%d (%s)\n",
                    alt, endpoint_rc, libusb_error_name(endpoint_rc));
            libusb_release_interface(h, 0);
            libusb_close(h);
            libusb_exit(NULL);
            return 2;
        }
        if (pkt_env && *pkt_env) {
            if (iso_pkt_size > endpoint_capacity) {
                fprintf(stderr,
                        "[usb] requested packet length %d exceeds advertised capacity %d; refusing URB\n",
                        iso_pkt_size, endpoint_capacity);
                libusb_release_interface(h, 0);
                libusb_close(h);
                libusb_exit(NULL);
                return 2;
            }
        } else {
            iso_pkt_size = endpoint_capacity;
        }
        if (endpoint_superspeed && iso_pkt_size > endpoint_capacity) {
            fprintf(stderr, "[usb] packet length validation failed\n");
            libusb_release_interface(h, 0);
            libusb_close(h);
            libusb_exit(NULL);
            return 2;
        }
        iso_packets = (endpoint_capacity + iso_pkt_size - 1) / iso_pkt_size;
        if (iso_packets < 1) iso_packets = 1;
        if (packets_env && *packets_env) iso_packets = atoi(packets_env);
        if (iso_packets < 1) iso_packets = 1;
        fprintf(stderr, "[iso] packet length=%d descriptors/URB=%d\n",
                iso_pkt_size, iso_packets);
    }

    // 差分観測ログ: 5番目の引数をラベルに captures/reads-<label>.tsv へ全IN読みを記録
    const char* label = argc > 4 ? argv[4] : NULL;
    if (label) {
        char lp[256]; snprintf(lp, sizeof(lp), "captures/reads-%s.tsv", label);
        g_rdlog = fopen(lp, "w");
        if (g_rdlog) { fprintf(g_rdlog, "idx\twValue\tsetupOUT\tresponse\n"); fprintf(stderr, "[main] 読みログ: %s\n", lp); }
    }

    // HD60S_SKIP_INIT=1 で init 呪文 replay + burst を丸ごとスキップ
    // (HD60S が既に別セッションで init 済みなら、我々が触ると passthrough が壊れる仮説)
    const char* env_skip_init = getenv("HD60S_SKIP_INIT");
    int skip_init = (env_skip_init && env_skip_init[0] && env_skip_init[0] != '0' && env_skip_init[0] != 'n' && env_skip_init[0] != 'N');
    if (!skip_init) {
        // 環境変数 HD60S_INIT_TSV で init TSV path を切り替え可能
        const char* init_tsv = getenv("HD60S_INIT_TSV");
        if (!init_tsv || !*init_tsv) init_tsv = "analysis/init-p2-audio-fast.tsv";
        fprintf(stderr, "[main] init: %s\n", init_tsv);
        hd60s_replay_spell(h, init_tsv);
        if (g_rdlog) { fclose(g_rdlog); g_rdlog = NULL; }
    } else {
        fprintf(stderr, "[main] HD60S_SKIP_INIT=1: init replay 省略\n");
    }

    // HDMI受信の信号ロックを"実際に見て"待つ(2026-07-09特定: 9d:0x12 bit0x80)。
    // 引数3が負数なら旧来の固定秒数待ちにフォールバック(比較用)。
    int lock_wait = argc > 3 ? atoi(argv[3]) : 4;
    if (lock_wait < 0) {
        int fixed = -lock_wait;
        fprintf(stderr, "[main] (固定)HDMIロック待ち %d秒...\n", fixed);
        for (int s = 0; s < fixed; s++) { struct timeval t={1,0}; libusb_handle_events_timeout(NULL,&t); }
    } else {
        fprintf(stderr, "[main] HDMIロック待ち(実検知, 最大%ds)...\n", lock_wait);
        int locked = hd60s_wait_for_lock(h, lock_wait * 1000, 200);
        fprintf(stderr, locked ? "[main] ロック検出！\n" : "[main] タイムアウト(未ロックのまま続行)\n");
    }

    // 診断(rank2): 入力タイミングレジスタを読み解像度確認(9d bank)。期待=1920x1080@60。
    {
        struct { const char* name; unsigned char hi, lo; } regs[] = {
            {"HActive", 0x29, 0x28}, {"HTotal", 0x6b, 0x6a}, {"VTotal", 0x5c, 0x5b},
        };
        for (int k = 0; k < 3; k++) {
            unsigned char o[3], in[1]; int v[2];
            for (int p = 0; p < 2; p++) {
                o[0]=0x9d; o[1]=0x01; o[2]= p ? regs[k].lo : regs[k].hi;
                libusb_control_transfer(h, 0x40, 192, 0x5066, 0, o, 3, 500);
                in[0]=0; libusb_control_transfer(h, 0xc0, 192, 0x5066, 0, in, 1, 500);
                v[p]=in[0];
            }
            fprintf(stderr, "[res] %s = %d (0x%02x%02x)\n", regs[k].name, (v[0]<<8)|v[1], v[0], v[1]);
        }
    }

    // パススルー専用モード: iso は張らず、poststream-full の全書込を pcap タイミング通りに撃つ。
    // 6番目引数が "release" なら poststream-full 撃った後に「IT66121解放シーケンス」10コマンド追加。
    // 解放シーケンス = pcap末尾に無い「AV_MUTE解除+SW_RST全解除+AFE fire+HDCP無効」の必須尾追い。
    // (2026-07-09 深夜 IT66121公開ドライバ(mainline it66121.c / HDZero / fl2000_drm)調査で判明)
    int do_release = (argc > 6 && strcmp(argv[6], "release") == 0);
    if (passthrough_only) {
        const char* pt_tsv = "analysis/poststream-full.tsv";
        if (argc > 6 && strcmp(argv[6], "nohdcp") == 0) {
            pt_tsv = "analysis/poststream-nohdcp.tsv";
            fprintf(stderr, "[pt-only] HDCP無効化モード\n");
        }
        fprintf(stderr, "[pt-only] %s を発火（iso 張らない）\n", pt_tsv);
        hd60s_load_burst(pt_tsv);
        hd60s_fire_burst(h);

        // 診断: IT66121 の 0x0E SYS_STATUS を読む (HPD/VID_STABLE 確認)
        {
            unsigned char setup[3] = {0x9b, 0x01, 0x0e};
            unsigned char val = 0;
            libusb_control_transfer(h, 0x40, 192, 0x5066, 0, setup, 3, 500);
            libusb_control_transfer(h, 0xc0, 192, 0x5066, 0, &val, 1, 500);
            fprintf(stderr, "[diag] IT66121 SYS_STATUS(0x0E)=0x%02x HPD=%d VID_STABLE=%d\n",
                    val, !!(val & 0x40), !!(val & 0x10));
        }

        // IT66121 解放シーケンス (2026-07-09 深夜, ITE公開ドライバ調査で判明)
        // pcap末尾は AV_MUTE ON+SW_RST 残置で終わってた=Windowsは追加コマンドで解放してた
        if (do_release) {
            fprintf(stderr, "[pt-only] IT66121 解放シーケンス発火 (AV_MUTE OFF, SW_RST 全解除, AFE fire, HDCP無効)\n");
            static const unsigned char release_seq[][3] = {
                {0x9a, 0x0f, 0x00},  // bank 0 選択(保険)
                {0x9a, 0x04, 0x00},  // SW_RST 全部解除 (VID/REF/AUD/AREF/HDCP)
                {0x9a, 0x62, 0x18},  // AFE_XP: RESETB=1, ENO=1
                {0x9a, 0x64, 0x94},  // AFE_IP: RESETB=1, GAINBIT=1, CKSEL_1 (>80MHz)
                {0x9a, 0x68, 0x00},  // LOWCLK clear
                {0x9a, 0x61, 0x00},  // AFE FIRE: RST=0 PWD=0 = TMDS 出力開始
                {0x9a, 0x20, 0x00},  // HDCP CPDESIRED=0 = 認証不要
                {0x9a, 0xc0, 0x01},  // HDMI モード (bit0=1)
                {0x9a, 0xc1, 0x00},  // AV MUTE OFF
                {0x9a, 0xc6, 0x03},  // Packet Gen ON + RPT
            };
            int nr = sizeof(release_seq) / 3;
            for (int i = 0; i < nr; i++) {
                int r = libusb_control_transfer(h, 0x40, 192, 0x5066, 0,
                                                (unsigned char*)release_seq[i], 3, 500);
                fprintf(stderr, "[release] %d/%d 9a %02x %02x r=%d\n", i+1, nr,
                        release_seq[i][1], release_seq[i][2], r);
                usleep(2000);
            }
            // 再診断
            unsigned char setup2[3] = {0x9b, 0x01, 0x0e};
            unsigned char val2 = 0;
            libusb_control_transfer(h, 0x40, 192, 0x5066, 0, setup2, 3, 500);
            libusb_control_transfer(h, 0xc0, 192, 0x5066, 0, &val2, 1, 500);
            fprintf(stderr, "[diag] SYS_STATUS after release = 0x%02x HPD=%d VID_STABLE=%d\n",
                    val2, !!(val2 & 0x40), !!(val2 & 0x10));
        }

        fprintf(stderr, "[pt-only] %d秒維持中...モニタ確認して\n", read_sec);
        for (int s = 0; s < read_sec; s++) sleep(1);
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(NULL);
        return 0;
    }

    // 点火バーストをロード(iso開始後に相対タイミングで発行)
    // HD60S_SKIP_BURST=1 で poststream burst 丸ごとスキップ (passthrough 保護)
    const char* env_skip_burst = getenv("HD60S_SKIP_BURST");
    int skip_burst = skip_init || (env_skip_burst && env_skip_burst[0] && env_skip_burst[0] != '0' && env_skip_burst[0] != 'n' && env_skip_burst[0] != 'N');
    if (!skip_burst) {
        const char* burst_tsv = getenv("HD60S_BURST_TSV");
        if (!burst_tsv || !*burst_tsv) burst_tsv = "analysis/poststream-no9a.tsv";
        hd60s_load_burst(burst_tsv);
    } else {
        fprintf(stderr, "[main] burst load 省略\n");
    }

    // 🔥 PASSTHROUGH PRE-ISO ENABLE (2026-07-11 Fable + kusq webcam 実験)
    // Windows pcap の enable trio 1 回目は **iso 開始 384ms 前** に発射される。
    // つまり init TSV 完了後、alt=2 の前 = ここで発射する必要ある。常に有効。
    {
        fprintf(stderr, "[pt-pre-iso] enable trio 1st fire (pre-alt=2)...\n");
        // (0) aa 12 34 90 05 00
        unsigned char t0[6] = {0xaa, 0x12, 0x34, 0x90, 0x05, 0x00};
        libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, t0, 6, 1000);
        // (1) aa 12 34 90 03 00
        unsigned char t1[6] = {0xaa, 0x12, 0x34, 0x90, 0x03, 0x00};
        libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, t1, 6, 1000);
        // (2) d4 00 04 03 (CPLD routing enable)
        unsigned char t2[4] = {0xd4, 0x00, 0x04, 0x03};
        libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, t2, 4, 1000);
        // (3) 5098 data=[0x20, 0x05] LE (Windows でも観測)
        unsigned char t3[2] = {0x20, 0x05};
        libusb_control_transfer(h, 0x40, 0xC0, 0x5098, 0, t3, 2, 1000);
        usleep(50000);  // 50ms sleep before alt=2 (Windows-observed)
    }

    int alt_rc = libusb_set_interface_alt_setting(h, 0, alt);
    if (alt_rc < 0) {
        fprintf(stderr, "set_alt(%d) failed rc=%d (%s)\n", alt, alt_rc,
                libusb_error_name(alt_rc));
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(NULL);
        return 2;
    }
    fprintf(stderr, "[main] alt=%d 選択OK\n", alt);

    // Diagnostic path for the USB 3 bulk alternate (alt=4).  The public
    // driver only exercised isochronous alternates; this bounded probe lets
    // us determine whether 0fd9:005e exposes media through bulk transport.
    if (alt == 4) {
        unsigned char bulk_buf[1024 * 32];
        int nonempty = 0;
        FILE *bulk_dump = fopen("/tmp/hd60s-bulk-stream.bin", "wb");
        hd60s_v4l2_open("/dev/video42");
        int bulk_count = 200;
        const char *bulk_env = getenv("HD60S_BULK_COUNT");
        if (bulk_env && *bulk_env) bulk_count = atoi(bulk_env);
        if (bulk_count < 1) bulk_count = 1;
        for (int n = 0; n < bulk_count; ++n) {
            int actual = 0;
            int rc = libusb_bulk_transfer(h, EP_STREAM, bulk_buf,
                                          (int)sizeof(bulk_buf), &actual, 250);
            if (rc == 0 && actual > 0) {
                if (bulk_dump) fwrite(bulk_buf, 1, (size_t)actual, bulk_dump);
                hd60s_parser_feed(bulk_buf, actual);
                total_bytes += actual;
                if (g_diag) g_diag_input_bytes += actual;
                pkt_ok++;
                if (nonempty++ < 8) {
                    fprintf(stderr, "[bulk] n=%d len=%d head=", n, actual);
                    for (int b = 0; b < actual && b < 16; ++b)
                        fprintf(stderr, "%02x", bulk_buf[b]);
                    fprintf(stderr, "\n");
                }
            } else if (rc != LIBUSB_ERROR_TIMEOUT && n < 8) {
                fprintf(stderr, "[bulk] n=%d rc=%d (%s) actual=%d\n",
                        n, rc, libusb_error_name(rc), actual);
            }
        }
        if (bulk_dump) fclose(bulk_dump);
        fprintf(stderr, "[bulk] nonempty_transfers=%d\n", nonempty);
        hd60s_v4l2_close();
        fprintf(stderr, "[bulk] bytes=%lld frames=%llu resyncs=%llu\n",
                total_bytes, g_frames_out, g_resyncs);
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(NULL);
        return nonempty ? 0 : 4;
    }
    // パススルー対策(H1): SET_INTERFACE → 初burstまで pcap実測 103ms 空ける (FX3 GPIF 再初期化とI2C競合回避)
    { struct timeval td={0, 120000}; libusb_handle_events_timeout(NULL, &td); }
    fprintf(stderr, "[main] iso開始 %d秒\n", read_sec);

    // IT6802E bank 0x94 audio setup: 0x0e=0x8b, 0x0f=0x8b, 0x10=0x05, 0x11=0x05, etc.
    // Must be applied before audio_open() and before submitting ISO transfers.
    // Active by default on the stable path (opt-out with HD60S_POST_ISO_AUDIO94=0).
    const char* env_pia94 = getenv("HD60S_POST_ISO_AUDIO94");
    int do_pia94 = !(env_pia94 && (env_pia94[0] == '0' || env_pia94[0] == 'n' || env_pia94[0] == 'N'));
    if (do_pia94) {
        hd60s_apply_it6802_audio94(h);
    }

    // v4l2loopback (/dev/video42) をオープン。失敗しても続行(生ストリームだけ保存)。
    hd60s_v4l2_open("/dev/video42");
    // ALSA snd-aloop hw:10,0 をオープン。環境変数 HD60S_ALSA_DEV でデバイス指定可能。
    const char* alsa_dev = getenv("HD60S_ALSA_DEV");
    if (!alsa_dev) alsa_dev = "hw:10,0";
    hd60s_audio_open(alsa_dev);

    outf = fopen("captures/stream-iso.bin", "wb");
    if (!outf) fprintf(stderr, "生ストリーム出力ファイル開けず(続行)\n");

    // firmware解析結果に基づき 0x509c (MCU bridge) audio init sequence を replay
    // pcap init-timed t=6.5-7.2s から抽出した 236個の 2byte writes
    // 環境変数 HD60S_509C=1 で有効化
    // 環境変数は "1"/"yes"/"on" で有効化 (getenv() != NULL だと "0" でも ON になり直感反するため)
    const char* env_509c = getenv("HD60S_509C");
    if (env_509c && env_509c[0] && env_509c[0] != '0' && env_509c[0] != 'n' && env_509c[0] != 'N') {
        fprintf(stderr, "[509c-init] MCU audio init sequence (236 writes)...\n");
    /* total 236 2-byte writes */
    static const unsigned short audio_init_seq[] = {
        0x0000, 0x1308, 0x0000, 0x0000, 0xb702, 0x0000, 0x416f, 0x0000, 0xb800, 0x0001, 0x0f02, 0x0001,
        0x1630, 0x0001, 0x1700, 0x0001, 0x1800, 0x0001, 0x1900, 0x0001, 0x1a50, 0x0001, 0x0001, 0x2a07,
        0x0002, 0x0803, 0x0001, 0x2500, 0x0001, 0x2600, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001, 0x0001,
        0x2440, 0x0001, 0x0001, 0x3080, 0x0001, 0x3100, 0x0001, 0x3200, 0x0000, 0xb014, 0x0000, 0x0000,
        0xae04, 0x0000, 0xad05, 0x0000, 0xb1c0, 0x0000, 0xb200, 0x0000, 0xb300, 0x0000, 0xb455, 0x0000,
        0x0000, 0xb454, 0x0002, 0x0161, 0x0002, 0x02f5, 0x0002, 0x0002, 0x0302, 0x0002, 0x0401, 0x0002,
        0x0500, 0x0002, 0x0608, 0x0002, 0x1c1a, 0x0002, 0x1d00, 0x0002, 0x1e00, 0x0002, 0x1f00, 0x0002,
        0x0002, 0x25a2, 0x0002, 0x0002, 0x02f5, 0x0002, 0x0002, 0x0704, 0x0002, 0x17c0, 0x0002, 0x19ff,
        0x0002, 0x1aff, 0x0002, 0x1bfc, 0x0002, 0x2000, 0x0002, 0x0002, 0x2100, 0x0002, 0x2226, 0x0002,
        0x2700, 0x0002, 0x0002, 0x2ea1, 0x0000, 0x0000, 0xab15, 0x0000, 0x0000, 0xac95, 0x0000, 0x0000,
        0xb702, 0x0000, 0xb810, 0x0000, 0xb800, 0x0002, 0x07f4, 0x0002, 0x0704, 0x0000, 0x5189, 0x0000,
        0x0000, 0xb700, 0x0000, 0xb700, 0x0002, 0x0002, 0x0161, 0x0002, 0x0002, 0x0401, 0x0002, 0x0608,
        0x0002, 0x0002, 0x0928, 0x0000, 0x0000, 0x5420, 0x0000, 0x0000, 0xac95, 0x0000, 0x0000, 0x0080,
        0x0000, 0x0000, 0xce80, 0x0000, 0x0000, 0xcf02, 0x0000, 0x0000, 0x0000, 0x0080, 0x0080, 0x0080,
        0xd000, 0x0080, 0xcf00, 0x0002, 0x0000, 0x0000, 0xab15, 0x0000, 0x0000, 0xac95, 0x0000, 0xad05,
        0x0000, 0x1e11, 0x0000, 0x1f01, 0x0000, 0x9c9f, 0x0000, 0x9b0b, 0x0000, 0x9674, 0x0000, 0x9503,
        0x0000, 0xa22c, 0x0000, 0xa101, 0x0000, 0x9a94, 0x0000, 0x9978, 0x0000, 0x942d, 0x0000, 0x9308,
        0x0000, 0xa040, 0x0000, 0x9f7f, 0x0000, 0x9eb3, 0x0000, 0x9d79, 0x0000, 0x9822, 0x0000, 0x977e,
        0x0000, 0xa42d, 0x0000, 0xa308, 0x0000, 0xa600, 0x0000, 0xa520, 0x0000, 0xa800, 0x0000, 0xa700,
        0x0000, 0xaa00, 0x0000, 0xa920, 0x0000, 0x0001, 0x0002, 0x27ff,
    };
        int seq_len = sizeof(audio_init_seq)/sizeof(audio_init_seq[0]);
        int ok = 0;
        for (int u = 0; u < seq_len; u++) {
            unsigned char d[2] = { audio_init_seq[u] & 0xff, (audio_init_seq[u] >> 8) & 0xff };
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, d, 2, 100);
            if (r == 2) ok++;
            usleep(500);  // pcap 実測 500us 間隔
        }
        fprintf(stderr, "[509c-init] %d/%d ok\n", ok, seq_len);
    }

    // IT6802E 0x94 bank audio unmute (Fable 発見 + IT6604 register spec 参考)
    // reg 0x87 (HWMUTE_CTRL) と reg 0x89 (TRISTATE_CTRL) を叩いて I2S 出力 untri-state
    // 環境変数 HD60S_AUDIO=1 で有効化
    const char* env_audio = getenv("HD60S_AUDIO");
    if (env_audio && env_audio[0] && env_audio[0] != '0' && env_audio[0] != 'n' && env_audio[0] != 'N') {
        fprintf(stderr, "[audio-unmute] IT6802E (0x94 bank) audio path unmute...\n");
        // IT6802E に一連のコマンドを送る (bank select → HWMUTE_CTRL clear → TRISTATE_CTRL clear)
        struct { unsigned char slave, reg, val; const char* name; } audio_unmute[] = {
            {0x94, 0x0f, 0x8b, "reg 0x0f audio clock enable"},
            // reg 0x87 (REG_RX_HWMUTE_CTRL): bit3=HW_MUTE_EN, bit4=MUTE_CLR
            //   = 0x10 → クリア (bit4 立てて bit3 クリア)
            {0x94, 0x87, 0x10, "reg 0x87 HWMUTE clear + disable"},
            // reg 0x89 (REG_RX_TRISTATE_CTRL): 全 I2S/SPDIF を untri-state
            //   = 0x00 → 全 clear
            {0x94, 0x89, 0x00, "reg 0x89 TRISTATE untri all"},
        };
        for (int u = 0; u < (int)(sizeof(audio_unmute)/sizeof(audio_unmute[0])); u++) {
            unsigned char w[3] = {audio_unmute[u].slave, audio_unmute[u].reg, audio_unmute[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 200);
            fprintf(stderr, "  %s = %d\n", audio_unmute[u].name, r);
            usleep(2000);
        }
    }

    // IT6802E audio unmute V2 — 正しい register (DB_C10 SDK it680x_regs.h 参照)
    // 環境変数 HD60S_AUDIO_V2=1 で有効化
    // Fable が参考にした IT6604 spec は reg 0x87 だが、IT6802 (実際の HD60S 用チップ) の
    // REG_RX_HWMuteCtrl は 0x7D。それ以外の関連 reg も一緒に叩く。
    const char* env_audio_v2 = getenv("HD60S_AUDIO_V2");
    if (env_audio_v2 && env_audio_v2[0] && env_audio_v2[0] != '0' && env_audio_v2[0] != 'n' && env_audio_v2[0] != 'N') {
        fprintf(stderr, "[audio-unmute-v2] IT6802E (正しい reg) audio path unmute...\n");
        struct { unsigned char slave, reg, val; const char* name; } audio_unmute_v2[] = {
            {0x94, 0x0f, 0x8b, "reg 0x0f audio clock enable"},
            // REG_RX_HWMuteCtrl = 0x7D
            //   bit3=HWMuteEn (0=disable HW mute), bit4=HWMuteClr (1=clear mute)
            //   0x10 = bit4 set, bit3 clear
            {0x94, 0x7d, 0x10, "reg 0x7D REG_RX_HWMuteCtrl clear"},
            // REG_RX_074 (reg 0x74): bit2=Force_AVMute (0=clear), bit3=AVMute_Value (0)
            //   全部 0 でクリア
            {0x94, 0x74, 0x00, "reg 0x74 Force_AVMute clear"},
            // REG_RX_0A8 (reg 0xA8): bit0=P0_AVMUTE, bit4=P1_AVMUTE → 全部 clear
            {0x94, 0xa8, 0x00, "reg 0xA8 AVMute (P0/P1) clear"},
            // REG_RX_07E (reg 0x7E): bit4=Force_I2SOut (=1 で強制 I2S 出力 ON)
            {0x94, 0x7e, 0x10, "reg 0x7E Force I2SOut ON"},
        };
        for (int u = 0; u < (int)(sizeof(audio_unmute_v2)/sizeof(audio_unmute_v2[0])); u++) {
            unsigned char w[3] = {audio_unmute_v2[u].slave, audio_unmute_v2[u].reg, audio_unmute_v2[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 200);
            fprintf(stderr, "  %s = %d\n", audio_unmute_v2[u].name, r);
            usleep(2000);
        }
    }

    // 最小 unmute: 0x509c で bank2 select → reg 0x20 = 0x00
    // 環境変数 HD60S_MIN=1 で有効化
    const char* env_min = getenv("HD60S_MIN");
    if (env_min && env_min[0] && env_min[0] != '0' && env_min[0] != 'n' && env_min[0] != 'N') {
        fprintf(stderr, "[min-unmute] 0x509c 最小シーケンス (bank2 select + reg 0x20=0x00)...\n");
        // firmware解析より: dev=2 page reg 0x20 = 0x00 が unmute
        // protocol: `00 BB` = bank切替 (BB=bank), `RR VV` = reg RR = VV
        // 発行 6回 (状態安定のため)
        for (int cycle = 0; cycle < 6; cycle++) {
            unsigned char sel[2] = {0x00, 0x02};
            libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, sel, 2, 100);
            usleep(500);
            unsigned char wr[2] = {0x20, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, wr, 2, 100);
            usleep(500);
        }
        fprintf(stderr, "  min-unmute done\n");
    }

    // firmware解析結果に基づき IT6802E (0x9c bank) audio unmute 直接I2C書き込み
    // 環境変数 HD60S_UNMUTE=1 で有効化 (legacy)
    const char* env_unmute = getenv("HD60S_UNMUTE");
    if (env_unmute && env_unmute[0] && env_unmute[0] != '0' && env_unmute[0] != 'n' && env_unmute[0] != 'N') {
        fprintf(stderr, "[unmute] IT6802E audio unmute シーケンス投入...\n");
        struct { unsigned char reg, val; const char* name; } audio_unmute[] = {
            {0x1B, 0xFF, "reg 0x1B ch enable all"},
            {0x02, 0xF5, "reg 0x02 AUD_EN (bit7 set)"},
            {0x27, 0x00, "reg 0x27 output mute clear"},
            {0x07, 0x04, "reg 0x07 audio path enable"},
            {0x25, 0xA2, "reg 0x25 audio ctrl"},
        };
        for (int u = 0; u < (int)(sizeof(audio_unmute)/sizeof(audio_unmute[0])); u++) {
            unsigned char w[3] = {0x9c, audio_unmute[u].reg, audio_unmute[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 1000);
            fprintf(stderr, "  %s = %d\n", audio_unmute[u].name, r);
            usleep(2000);
        }
        // IT66121 (0x9a) 側 audio ctrl も unmute
        struct { unsigned char reg, val; const char* name; } tx_audio[] = {
            {0xC1, 0x00, "reg 0xC1 AVMUTE clear"},
            {0xB9, 0x03, "reg 0xB9 audio InfoFrame enable"},
            {0xE0, 0x01, "reg 0xE0 audio ctrl"},
            {0xE4, 0x10, "reg 0xE4 audio config"},
        };
        for (int u = 0; u < (int)(sizeof(tx_audio)/sizeof(tx_audio[0])); u++) {
            unsigned char w[3] = {0x9a, tx_audio[u].reg, tx_audio[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 1000);
            fprintf(stderr, "  IT66121 %s = %d\n", tx_audio[u].name, r);
            usleep(2000);
        }
    }

    size_t transfer_bytes = (size_t)iso_packets * (size_t)iso_pkt_size;
    if (transfer_bytes == 0 || transfer_bytes > INT_MAX) {
        fprintf(stderr, "[iso] invalid transfer buffer size=%zu\n", transfer_bytes);
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(NULL);
        return 2;
    }
    struct libusb_transfer* xfrs[NUM_TRANSFERS] = {0};
    unsigned char* bufs[NUM_TRANSFERS] = {0};
    unsigned char devmem[NUM_TRANSFERS] = {0};
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        // Zerocopy DMA バッファ (usbfs mmap経由) → CPU 使用率↓、tail latency↓。
        // 失敗時は malloc にフォールバック (小型ホストで KMS が確保できない場合)。
        bufs[i] = libusb_dev_mem_alloc(h, (int)transfer_bytes);
        if (bufs[i]) devmem[i] = 1;
        if (!bufs[i]) bufs[i] = malloc(transfer_bytes);
        if (!bufs[i]) {
            usb_session_fatal = 1;
            usb_session_error = LIBUSB_ERROR_NO_MEM;
            fprintf(stderr, "[iso] buffer allocation failed at transfer %d\n", i);
            break;
        }
        xfrs[i] = libusb_alloc_transfer(iso_packets);
        if (!xfrs[i]) {
            usb_session_fatal = 1;
            usb_session_error = LIBUSB_ERROR_NO_MEM;
            fprintf(stderr, "[iso] transfer allocation failed at transfer %d\n", i);
            break;
        }
        // timeout=0 = 無限。連続isoで有限timeoutはURBキャンセルで in-flight packet 全て empty化する罠
        libusb_fill_iso_transfer(xfrs[i], h, EP_STREAM, bufs[i],
            (int)transfer_bytes, iso_packets, iso_cb, NULL, 0);
        libusb_set_iso_packet_lengths(xfrs[i], iso_pkt_size);
        int submit_rc = libusb_submit_transfer(xfrs[i]);
        if (submit_rc == 0) {
            inflight++;
            submit_ok++;
            if (inflight > max_inflight) max_inflight = inflight;
        } else {
            submit_fail++;
            fprintf(stderr, "submit%d failed rc=%d (%s)\\n", i, submit_rc, libusb_error_name(submit_rc));
        }
    }
    fprintf(stderr, "[main] iso転送 %d 本投入\n", inflight);

    fprintf(stderr, "[iso] submit summary ok=%lu fail=%lu max_inflight=%d current=%d\\n",
            submit_ok, submit_fail, max_inflight, inflight);
    if (inflight == 0 || usb_session_fatal) {
        usb_session_fatal = 1;
        if (!usb_session_error) usb_session_error = LIBUSB_ERROR_OTHER;
        keep_running = 0;
        fprintf(stderr, "[iso] no active transfers or allocation failure; ending USB session before capture\\n");
        goto capture_cleanup;
    }

    // 🔍 IT66121 状態 dump (パススルー sequence 前後の切り分け用)
    // マクロは file 後半で定義されてるので inline で書く
    {
        unsigned char regs[16] = {0};
        for (int r = 0; r < 16; r++) {
            unsigned char setup[3] = {0x9b, 0x01, (unsigned char)r};
            unsigned char resp[1] = {0};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, setup, 3, 500);
            int rr = libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, resp, 1, 500);
            regs[r] = (rr > 0) ? resp[0] : 0xff;
            usleep(1000);
        }
        fprintf(stderr, "[TX before-pt] 0x00-0x0f:");
        for (int r = 0; r < 16; r++) fprintf(stderr, " %02x", regs[r]);
        fprintf(stderr, "  (0x04=%02x → SW_RST)\n", regs[4]);
    }

    // 🔥 HDMI PASSTHROUGH ENABLE (2026-07-11 Fable 3rd 分析)
    // 真の犯人発見: IT66121 TX (slave 0x9a) は最初から完璧。真のパススルー enable は
    // MCU (slave 0xaa magic 12 34) と CPLD (slave 0xd4) 経由の "秘密コマンド 6 発"。
    // Windows はこれを 3 回発射する。「reg 0x27 = video gate」を開いて RX→TX ルート
    // を CPLD で有効化する。
    // 前バージョン (15 writes to 0x9a) は TX しか触ってなかったので RX→TX の物理路
    // が CPLD で切れたまま = TV 無信号だった。
    {
        int pt_ok = 0, pt_fail = 0;
        #define TX_WRITE(reg, val) do { \
            unsigned char _w[3] = {0x9a, (reg), (val)}; \
            int _r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, _w, 3, 500); \
            if (_r == 3) pt_ok++; else pt_fail++; \
        } while (0)

        // 🎯 真のパススルー enable シーケンス (Fable 3rd 分析)
        // Windows は 3 回発射するのでここでも 3 回発射
        fprintf(stderr, "[passthrough] TRUE enable sequence (MCU+CPLD, 6 cmds × 3 rounds)...\n");
        for (int round = 0; round < 3; round++) {
            // 1. MCU reg 0x27 "video gate" open
            {
                unsigned char w[2] = {0x27, 0x00};
                int r = libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, w, 2, 500);
                if (r == 2) pt_ok++; else pt_fail++;
            }
            // 2. MCU RPC set-mode param 5
            {
                unsigned char w[6] = {0xaa, 0x12, 0x34, 0x90, 0x05, 0x00};
                int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 6, 500);
                if (r == 6) pt_ok++; else pt_fail++;
            }
            // 3. MCU RPC set-mode param 3
            {
                unsigned char w[6] = {0xaa, 0x12, 0x34, 0x90, 0x03, 0x00};
                int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 6, 500);
                if (r == 6) pt_ok++; else pt_fail++;
            }
            // 4. CPLD routing reg 0x04 = 0x03 (bit0=RX→TX, bit1=RX→FX3 両方 enable)
            {
                unsigned char w[4] = {0xd4, 0x00, 0x04, 0x03};
                int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 4, 500);
                if (r == 4) pt_ok++; else pt_fail++;
            }
            // 5. CPLD keepalive
            {
                unsigned char w[4] = {0xd4, 0x00, 0x2a, 0x6e};
                int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 4, 500);
                if (r == 4) pt_ok++; else pt_fail++;
            }
            // 6. CPLD keepalive
            {
                unsigned char w[4] = {0xd4, 0x00, 0x01, 0x02};
                int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 4, 500);
                if (r == 4) pt_ok++; else pt_fail++;
            }
            usleep(500 * 1000);  // 500ms 間隔で 3 回
        }
        fprintf(stderr, "[passthrough] MCU/CPLD enable 統計: ok=%d fail=%d\n", pt_ok, pt_fail);
        pt_ok = 0; pt_fail = 0;

        // 🔥 0x9a TX writes 全部削除 (2026-07-11 Fable 4th 分析)
        // HD60S は既定でパススルー ON、MCU が自律で IT66121 を制御する。
        // ホストから 0x9a に書き込むと MCU 設定を壊す (friendly fire) → TMDS 不安定 →
        // TV が「省電力モーダル」に落ちる。MCU/CPLD 制御 (aa/d4) だけ残し、
        // 0x9a への書き込みは一切しない。
        (void)pt_ok; (void)pt_fail;
        #undef TX_WRITE
    }
    // 🔍 IT66121 状態 dump (パススルー sequence 直後)
    {
        unsigned char regs[16] = {0};
        for (int r = 0; r < 16; r++) {
            unsigned char setup[3] = {0x9b, 0x01, (unsigned char)r};
            unsigned char resp[1] = {0};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, setup, 3, 500);
            int rr = libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, resp, 1, 500);
            regs[r] = (rr > 0) ? resp[0] : 0xff;
            usleep(1000);
        }
        fprintf(stderr, "[TX after-pt] 0x00-0x0f:");
        for (int r = 0; r < 16; r++) fprintf(stderr, " %02x", regs[r]);
        fprintf(stderr, "  (0x04=%02x SW_RST, 0x0E=%02x SYS_STAT)\n", regs[4], regs[14]);
    }

    // 🔥 POST-ISO AUDIO CONFIG (2026-07-11 pcap RE breakthrough)
    // Windows sends 188 I2C writes to slave 0x9a (IT66121 TX) & 0x94 (audio bank)
    // in first 400ms after iso start. We never sent these — that's why audio dies at 100ms.
    // 環境変数 HD60S_POST_ISO_AUDIO=1 で有効化 (default off で回帰リスク低減)
    {
        const char* env_pia = getenv("HD60S_POST_ISO_AUDIO");
        int do_pia = (env_pia && env_pia[0] && env_pia[0] != '0' && env_pia[0] != 'n' && env_pia[0] != 'N');
        if (do_pia) {
            hd60s_apply_post_iso_audio(h);
        }

    }

    // IT6802E bank 0x94 audio setup (also applied before audio stream open).
    if (do_pia94) {
        hd60s_apply_it6802_audio94(h);
    }

    // iso をポンプしながら、点火バーストを相対タイムスタンプ準拠で発行する。
    // 単一スレッド: libusb_control_transfer は内部で iso 転送もポンプするので iso は止まらない。
    double burst_t0 = g_nburst ? g_burst[0].t : 0;
    double start = now_s();
    int bi = 0, bok = 0, bfail = 0;
    struct timeval tv = {0, g_pace_output ? 1000 : 10000}; // paced: 1ms wakeup
    const char* env_pt = getenv("HD60S_PT_LOOP");
    int pt_loop = (env_pt && env_pt[0] && env_pt[0] != '0' && env_pt[0] != 'n' && env_pt[0] != 'N');
    double last_pt_fire = 0.0;
    int pt_fires = 0;
    while (!g_stop_requested && keep_running && now_s() - start < read_sec && inflight > 0) {
        double el = now_s() - start;
        while (bi < g_nburst && (g_burst[bi].t - burst_t0) <= el) {
            BurstCmd* b = &g_burst[bi];
            unsigned char inbuf[80]; int r;
            if (b->is_out)
                r = libusb_control_transfer(h, b->brt, b->br, b->wv, b->wi, b->data, b->dlen, 1000);
            else
                r = libusb_control_transfer(h, b->brt, b->br, b->wv, b->wi, inbuf, b->wl, 1000);
            if (r < 0) bfail++; else bok++;
            bi++;
        }
        // AUDIO KEEPALIVE V2: 2026-07-11 Opus 4.8 サブエージェント発見。
        // Windows steady-state は 227コマンドの keepalive cycle を 163ms 周期で firing.
        // 環境変数 HD60S_AUDIO_KA=1 で有効化。keepalive TSV を別バッファに読み込んで再送。
        static double last_ka_fire = 0.0;
        static int ka_fires = 0;
        static BurstCmd g_ka[512];
        static int g_nka = -1;  // -1 = 未ロード
        const char* env_ka = getenv("HD60S_AUDIO_KA");
        int ka_loop = (env_ka && env_ka[0] && env_ka[0] != '0');
        if (ka_loop && g_nka < 0) {
            // 初回: TSV load (別関数使いたいが load_burst は g_burst を潰すので inline)
            FILE* fka = fopen("analysis/keepalive-cycle-v2.tsv", "r");
            if (!fka) { fprintf(stderr, "[ka] keepalive-cycle-v2.tsv 開けず、KA無効化\n"); g_nka = 0; ka_loop = 0; }
            else {
                char line[8192]; int first = 1; g_nka = 0;
                while (fgets(line, sizeof(line), fka) && g_nka < 512) {
                    if (first) { first = 0; continue; }
                    char cf[32], ct[32], cbrt[16], cbr[16], cwv[16], cwi[16], cwl[16], cd[8000];
                    cd[0] = 0;
                    int nf = sscanf(line, "%31[^\t]\t%31[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%7999[^\t\n]",
                                    cf, ct, cbrt, cbr, cwv, cwi, cwl, cd);
                    if (nf < 7) continue;
                    BurstCmd* c = &g_ka[g_nka];
                    c->t = 0; c->brt = (unsigned char)strtol(cbrt, NULL, 0);
                    c->br = (unsigned char)strtol(cbr, NULL, 10);
                    c->wv = (unsigned short)strtol(cwv, NULL, 0);
                    c->wi = (unsigned short)strtol(cwi, NULL, 0);
                    c->wl = (unsigned short)strtol(cwl, NULL, 10);
                    c->is_out = (c->brt & 0x80) == 0;
                    c->dlen = 0;
                    if (c->is_out && nf >= 8 && cd[0]) {
                        c->dlen = hex2bin(cd, c->data, sizeof(c->data));
                    }
                    g_nka++;
                }
                fclose(fka);
                fprintf(stderr, "[ka] keepalive-cycle-v2.tsv: %d commands loaded\n", g_nka);
            }
        }
        // KA fires INDEPENDENT of burst completion — audio dies at 100ms so we can't wait for burst
        if (ka_loop && g_nka > 0 && (el - last_ka_fire) >= 0.163) {
            unsigned char inbuf[80];
            int ka_ok = 0;
            for (int k = 0; k < g_nka; k++) {
                BurstCmd* c = &g_ka[k];
                int r;
                if (c->is_out) r = libusb_control_transfer(h, c->brt, c->br, c->wv, c->wi, c->data, c->dlen, 100);
                else r = libusb_control_transfer(h, c->brt, c->br, c->wv, c->wi, inbuf, c->wl, 100);
                if (r >= 0) ka_ok++;
            }
            last_ka_fire = el;
            ka_fires++;
            if (ka_fires <= 3) fprintf(stderr, "[ka] cycle #%d fired: %d/%d ok\n", ka_fires, ka_ok, g_nka);
        }

        // 🔥 IT6802 AUDIO RECOVERY LOOP (2026-07-11 Fable + FIX_ID_023 breakthrough)
        // ITE公式ドライバ it680x.c の AudioFsCal() + aud_fiforst() + Force FS を再現。
        // 100ms周期で HW mute解除 + 48kHz強制 + I2S untri-state を再送。
        // HD60S_IT6802_RECOVER=1 で有効化。IT6802 access = I2C slave 0x94 (write) bank 0
        static double last_it6802_rec = 0.0;
        static int it6802_rec_fires = 0;
        const char* env_rec = getenv("HD60S_IT6802_RECOVER");
        int do_rec = (env_rec && env_rec[0] && env_rec[0] != '0' && env_rec[0] != 'n' && env_rec[0] != 'N');
        static double rec_interval = -1.0;
        if (rec_interval < 0) {
            const char* env_int = getenv("HD60S_RECOVER_MS");
            rec_interval = (env_int && atoi(env_int) > 0) ? atoi(env_int) / 1000.0 : 0.100;
        }
        // t=0 でも即発火 (last_it6802_rec == 0.0 で初回、初期無音を潰す)
        if (do_rec && (last_it6802_rec == 0.0 || (el - last_it6802_rec) >= rec_interval)) {
            #define IT6802W(reg, val) do { \
                unsigned char _w[3] = {0x94, (reg), (val)}; \
                libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, _w, 3, 100); \
            } while (0)
            // 1) Maintain verified bank 0x94 audio config (0x0e=0x8b, 0x0f=0x8b, 0x10=0x05, 0x11=0x05)
            IT6802W(0x0e, 0x8b);
            IT6802W(0x0f, 0x8b);
            IT6802W(0x10, 0x05);
            IT6802W(0x11, 0x05);
            // 2) HW mute clear: REG_RX_HWMuteCtrl(0x7D):  bit4=HWMuteClr, bit5=HWAudMuteClrMode
            IT6802W(0x7d, 0x30);   // set both
            IT6802W(0x7d, 0x00);   // clear both
            // 3) Keep I2S/SPDIF enabled and auto-detect sampling frequency (0xA0 = I2S+SPDIF ON, Auto FS)
            IT6802W(0x74, 0xa0);
            // 4) REG_RX_075 = 0x40: Audio 24bit → 16bit conversion (ITE init default)
            IT6802W(0x75, 0x40);
            // 5) REG_RX_07E: clear B_HBRSel (bit6) for standard audio
            IT6802W(0x7e, 0x00);
            // 6) un-tristate I2S+SPDIF: REG_RX_052
            IT6802W(0x52, 0x20);
            #undef IT6802W
            last_it6802_rec = el;
            it6802_rec_fires++;
            if (it6802_rec_fires <= 3) fprintf(stderr, "[it6802-rec] fire #%d at t=%.0fms\n", it6802_rec_fires, el*1000);
        }

        // 🔥 PASSTHROUGH KEEPALIVE (2026-07-11 Fable + kusq webcam 検証)
        // MCU/CPLD の d4 slave keepalive を 100ms 周期で発射しないと LG モニターが
        // 「省電力モーダル」に落ちる (パススルー ON→OFF の切れ目を検知される)。
        // Windows pcap では 40-300ms 周期で連続発射している。
        static double last_pt_ka = 0.0;
        static int pt_ka_fires = 0;
        // In paced capture this synchronous five-transfer maintenance cycle
        // blocks the same thread that must submit V4L2 frames at 60 Hz.  The
        // passthrough keepalive is retained for unpaced/pass-through runs,
        // but must not stall the capture presentation clock.
        if (!g_pace_output && (el - last_pt_ka) >= 0.100) {
            // enable trio + keepalive pair を毎回発射
            // aa 12 34 90 05 00
            unsigned char w0a[6] = {0xaa, 0x12, 0x34, 0x90, 0x05, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w0a, 6, 100);
            // aa 12 34 90 03 00
            unsigned char w0b[6] = {0xaa, 0x12, 0x34, 0x90, 0x03, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w0b, 6, 100);
            // d4 00 04 03 (CPLD routing)
            unsigned char w0c[4] = {0xd4, 0x00, 0x04, 0x03};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w0c, 4, 100);
            // d4 00 2a 6e
            unsigned char w1[4] = {0xd4, 0x00, 0x2a, 0x6e};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w1, 4, 100);
            // d4 00 01 02
            unsigned char w2[4] = {0xd4, 0x00, 0x01, 0x02};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w2, 4, 100);
            last_pt_ka = el;
            pt_ka_fires++;
            if (pt_ka_fires <= 3) fprintf(stderr, "[pt-keepalive] fire #%d at t=%.0fms (full trio+keepalive)\n", pt_ka_fires, el*1000);
        }

        // 60B MCU BATCH RETRY: iso 中に arm batch を反復発火して audio DMA restart 試行
        // 環境変数 HD60S_BATCH_LOOP=1 で有効化、80ms 周期
        static double last_batch_fire = 0.0;
        static int batch_fires = 0;
        const char* env_batch_loop = getenv("HD60S_BATCH_LOOP");
        int batch_loop = (env_batch_loop && env_batch_loop[0] && env_batch_loop[0] != '0');
        if (batch_loop && (el - last_batch_fire) >= 0.080) {
            static const unsigned char arm_payload[60] = {
                0x55, 0x80, 0x3c, 0x00, 0x6a, 0x80, 0x0f, 0x00, 0x6b, 0x80, 0xfe, 0x00,
                0x01, 0x81, 0x07, 0x00, 0x0b, 0x82, 0xdf, 0x00, 0x0c, 0x82, 0x3f, 0x00,
                0x0e, 0x82, 0x08, 0x00, 0x48, 0x82, 0x60, 0x00, 0x9b, 0x82, 0xf0, 0x00,
                0x11, 0x82, 0xff, 0x00, 0x12, 0x82, 0xff, 0x00, 0x11, 0x82, 0xff, 0x00,
                0x12, 0x82, 0xff, 0x00, 0x0e, 0x40, 0xd0, 0x9a, 0x0e, 0x40, 0x80, 0x9a
            };
            libusb_control_transfer(h, 0x40, 0xC6, 0x0000, 0x0100, NULL, 0, 100);
            libusb_control_transfer(h, 0x40, 0xC6, 0x0032, 0x0101, (unsigned char*)arm_payload, 60, 100);
            last_batch_fire = el;
            batch_fires++;
            if (batch_fires <= 3) fprintf(stderr, "[batch] fired #%d at t=%.0fms\n", batch_fires, el*1000);
        }

        // PLL LOCK MONITOR: 2026-07-11 Fable ヒント。IT6802 の IPLL_LOCK が
        // 音声死亡時に drop してるかを 20ms 周期で monitor。
        // 環境変数 HD60S_PLL_MON=1 で有効化
        static double last_pll_mon = 0.0;
        static int pll_mon_fires = 0;
        const char* env_pll_mon = getenv("HD60S_PLL_MON");
        int pll_mon = (env_pll_mon && env_pll_mon[0] && env_pll_mon[0] != '0');
        if (pll_mon && (el - last_pll_mon) >= 0.020) {
            // BREAKTHROUGH: slave 0x94 audio bank + read via 0x95 = real audio state
            // First select page 2 audio bank on slave 0x94
            static int first_time = 1;
            if (first_time) {
                unsigned char bank[3] = {0x94, 0x0f, 0x02};
                libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, bank, 3, 30);
                first_time = 0;
            }
            unsigned char vals[6] = {0};
            const unsigned char reads[][3] = {
                {0x95, 0x01, 0xaa},  // AUDIO_CH_STAT via correct slave
                {0x95, 0x01, 0xae},  // AUD_CHSTAT3 (M_FS)
                {0x95, 0x01, 0xad},  // channel/source count
                {0x95, 0x01, 0xac},  // channel status
                {0x95, 0x01, 0xab},  // channel status 0
                {0x95, 0x01, 0xa9},  // (unknown but returned 0x11 in probe)
            };
            for (int i = 0; i < 6; i++) {
                libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, (unsigned char*)reads[i], 3, 30);
                libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, &vals[i], 1, 30);
            }
            if (pll_mon_fires < 60) {
                fprintf(stderr, "[audio] t=%6.0fms AA=0x%02x(AUD_ON=%d HBR=%d LAYOUT=%d CH=%d) AE=0x%02x(Fs=%d) AD=0x%02x AC=0x%02x AB=0x%02x A9=0x%02x\n",
                        el*1000, vals[0],
                        (vals[0]>>7)&1, (vals[0]>>6)&1, (vals[0]>>4)&1, vals[0]&0x0F,
                        vals[1], vals[1]&0x0F,
                        vals[2], vals[3], vals[4], vals[5]);
            }
            last_pll_mon = el;
            pll_mon_fires++;
        }

        // AUDIO ARM RETRY: 2026-07-11 iso 中に arm sequence を反復投入。
        // 100ms で音声死ぬ→ もしかしたら arm 効果が 100ms しかもたない?
        // 環境変数 HD60S_ARM_LOOP=1 で有効化、30ms 周期
        static double last_arm_loop = 0.0;
        static int arm_loop_fires = 0;
        const char* env_arm_loop = getenv("HD60S_ARM_LOOP");
        int arm_loop = (env_arm_loop && env_arm_loop[0] && env_arm_loop[0] != '0');
        if (arm_loop && (el - last_arm_loop) >= 0.030) {
            unsigned char arm_a[4] = {0xd4, 0x00, 0x04, 0x03};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, arm_a, 4, 100);
            unsigned char arm_b[2] = {0x20, 0x05};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5098, 0, arm_b, 2, 100);
            last_arm_loop = el;
            arm_loop_fires++;
        }

        // AUDIO UNMUTE-RETRY: 2026-07-11 Fable ヒント。IT6802E は ACR unlock で
        // hard-mute 発動→ read-then-clear 必要。50ms 周期で mute clear を強制書き込み。
        // 環境変数 HD60S_UNMUTE_RETRY=1 で有効化
        static double last_unmute_fire = 0.0;
        static int unmute_fires = 0;
        const char* env_unmute_retry = getenv("HD60S_UNMUTE_RETRY");
        int unmute_retry = (env_unmute_retry && env_unmute_retry[0] && env_unmute_retry[0] != '0');
        if (unmute_retry && (el - last_unmute_fire) >= 0.050) {
            // Bank2 select (audio bank)
            unsigned char sel[3] = {0x94, 0x0f, 0x02};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, sel, 3, 100);
            // Force clear HWMuteCtrl (reg 0x7D bit4 = HWMuteClr trigger)
            unsigned char w1[3] = {0x94, 0x7d, 0x10};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w1, 3, 100);
            // Clear Force_AVMute (reg 0x74)
            unsigned char w2[3] = {0x94, 0x74, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w2, 3, 100);
            // Clear P0/P1_AVMUTE (reg 0xA8)
            unsigned char w3[3] = {0x94, 0xa8, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w3, 3, 100);
            // Force I2S output on (reg 0x7E bit4)
            unsigned char w4[3] = {0x94, 0x7e, 0x10};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w4, 3, 100);
            last_unmute_fire = el;
            unmute_fires++;
            if (unmute_fires <= 3) fprintf(stderr, "[unmute-retry] fired #%d\n", unmute_fires);
        }

        // min-unmute-loop: burst 完了後、100ms 周期で bank2 select + reg 0x20=0x00 を再送
        // 環境変数 HD60S_MIN_LOOP=1 で有効化 (P5 で peak +71% 確認済み)
        static double last_min_fire = 0.0;
        static int min_fires = 0;
        const char* env_min_loop = getenv("HD60S_MIN_LOOP");
        int min_loop = (env_min_loop && env_min_loop[0] && env_min_loop[0] != '0');
        if (min_loop && bi >= g_nburst && (el - last_min_fire) >= 0.10) {
            unsigned char sel[2] = {0x00, 0x02};
            libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, sel, 2, 50);
            unsigned char wr[2] = {0x20, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, wr, 2, 50);
            last_min_fire = el;
            min_fires++;
        }

        // pt-loop: burst 完了後、120ms 周期で **完全な** passthrough keep-alive cycle を発火
        // pcap 解析: 30 commands (9a 書き, 9b/9d 読み, 0x509c MCU) を 120ms 周期で全部やる
        // 詳細は analysis/keepalive-cycle.tsv 参照
        if (pt_loop && bi >= g_nburst && (el - last_pt_fire) >= 0.12) {
            // IT66121 writes: 9a0f00, 9ac101, 9ac603 (start of cycle)
            unsigned char w1[3] = {0x9a, 0x0f, 0x00}; libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w1, 3, 100);
            unsigned char w2[3] = {0x9a, 0xc1, 0x01}; libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w2, 3, 100);
            unsigned char w3[3] = {0x9a, 0xc6, 0x03}; libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w3, 3, 100);
            // IT66121 read setup for 9b/reg 0x0e (SYS_STATUS)
            unsigned char rs1[3] = {0x9b, 0x01, 0x0e};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, rs1, 3, 100);
            unsigned char rb[1];
            libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, rb, 1, 100);
            // MCU seq (0x509c series)
            unsigned short ka_seq[] = {
                0x0000, 0x0000, 0xb3ff, 0x0002, 0x27ff, 0x0002,
                0x0002, 0x0002, 0x0002, 0x0002, 0x2000
            };
            for (int k = 0; k < (int)(sizeof(ka_seq)/sizeof(ka_seq[0])); k++) {
                unsigned char d[2] = { ka_seq[k] & 0xff, (ka_seq[k] >> 8) & 0xff };
                libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, d, 2, 100);
                usleep(500);
            }
            // IT6802E reads: 0x11, 0x12 (interleaved with 0x509c 0x0002 - simplified)
            unsigned char rs2[3] = {0x9d, 0x01, 0x11};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, rs2, 3, 100);
            libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, rb, 1, 100);
            unsigned char rs3[3] = {0x9d, 0x01, 0x12};
            libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, rs3, 3, 100);
            libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, rb, 1, 100);
            last_pt_fire = el;
            pt_fires++;
    }
    int event_rc = libusb_handle_events_timeout(NULL, &tv);
    if (event_rc == LIBUSB_ERROR_NO_DEVICE || event_rc == LIBUSB_ERROR_IO ||
        event_rc == LIBUSB_ERROR_OTHER) {
        usb_session_fatal = 1;
        usb_session_error = event_rc;
        keep_running = 0;
        fprintf(stderr, "[iso] event handling failed rc=%d (%s)\\n",
                event_rc, libusb_error_name(event_rc));
    }
        hd60s_pace_output_if_due();
    }
    if (g_stop_requested) {
        keep_running = 0;
        fprintf(stderr, "[main] stop requested; draining USB transfers\n");
    }
    if (usb_session_fatal)
        fprintf(stderr, "[iso] USB session ended: status=%d (%s)\n",
                usb_session_error,
                usb_session_error < 0 ? libusb_error_name(usb_session_error) : "transfer status");
    if (usb_session_fatal)
        goto capture_cleanup;
    if (pt_loop) fprintf(stderr, "[pt-loop] 継続発火 %d 回\n", pt_fires);
    fprintf(stderr, "[burst] 発行 %d/%d (ok=%d fail=%d)\n", bi, g_nburst, bok, bfail);

    // The pre-burst endpoint carries control/blanking data that can contain
    // line-marker byte patterns.  Any parser lock obtained there is invalid
    // for the video phase.  Start a fresh sliding-window search immediately
    // after the stream-enable burst has completed.
    hd60s_parser_reset_video_phase();
    fprintf(stderr, "[parser] video-phase synchronization reset after burst\n");

    // (pt-loop は main iso loop 内でinline 実行、ここでの再ループは削除)

    // IT66121 SYS_STATUS 読み関数 (arm 前後比較用)
    // reg 0x0E は SYS_STATUS。ITE ドキュメント上 bit4=VID_STABLE
    #define IT66121_READ(reg) ({ \
        unsigned char _setup[3] = {0x9b, 0x01, (unsigned char)(reg)}; \
        libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, _setup, 3, 1000); \
        unsigned char _resp[8]; \
        int _rr = libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, _resp, 1, 1000); \
        (_rr > 0) ? _resp[0] : 0xff; \
    })

    // arm 前の状態記録 (複数 register を i2c read - 各読みの間 5ms 空ける)
    #define IT66121_SNAP(tag) do { \
        unsigned char _r[16]; \
        for (int _i = 0; _i < 16; _i++) { \
            _r[_i] = IT66121_READ(_i); \
            usleep(2000); \
        } \
        fprintf(stderr, "[%s] IT66121 reg dump 0x00-0x0f:", tag); \
        for (int _i = 0; _i < 16; _i++) fprintf(stderr, " %02x", _r[_i]); \
        fprintf(stderr, "\n"); \
    } while (0)

    IT66121_SNAP("pre-arm");

    // Workflow synth 提案: MCU arm 60B バッチ を明示的に再送 (frame 13573 のペイロード)
    {
        static const unsigned char arm_payload[60] = {
            0x55, 0x80, 0x3c, 0x00, 0x6a, 0x80, 0x0f, 0x00, 0x6b, 0x80, 0xfe, 0x00,
            0x01, 0x81, 0x07, 0x00, 0x0b, 0x82, 0xdf, 0x00, 0x0c, 0x82, 0x3f, 0x00,
            0x0e, 0x82, 0x08, 0x00, 0x48, 0x82, 0x60, 0x00, 0x9b, 0x82, 0xf0, 0x00,
            0x11, 0x82, 0xff, 0x00, 0x12, 0x82, 0xff, 0x00, 0x11, 0x82, 0xff, 0x00,
            0x12, 0x82, 0xff, 0x00, 0x0e, 0x40, 0xd0, 0x9a, 0x0e, 0x40, 0x80, 0x9a
        };
        int r1 = libusb_control_transfer(h, 0x40, 0xC6, 0x0000, 0x0100, NULL, 0, 1000);
        int r2 = libusb_control_transfer(h, 0x40, 0xC6, 0x0032, 0x0101,
                                         (unsigned char*)arm_payload, 60, 1000);
        fprintf(stderr, "[arm] setup=%d arm=%d (60B MCU batch)\n", r1, r2);
    }
    usleep(50 * 1000);
    IT66121_SNAP("post-arm");

    // 追加試行: IT66121 ドライバ(Linux mainline) の "FireAFE + HDMI mode + unmute" 手順を明示送信
    // ite-it66121.c より:
    //   0x61 = 0x00 (FireAFE: AFE 起動)
    //   0xC0 = 0x01 (HDMI mode enable)
    //   0xC1 = 0x00 (AV unmute)
    //   0xC6 = 0x03 (packet generation)
    //   0x0F bit4 clear (TX clock)
    #define IT66121_WRITE(reg, val) do { \
        unsigned char _w[3] = {0x9a, (reg), (val)}; \
        libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, _w, 3, 1000); \
    } while (0)

    fprintf(stderr, "[fire-afe] IT66121 output enable sequence...\n");
    IT66121_WRITE(0x61, 0x00); usleep(1000);   // FireAFE
    IT66121_WRITE(0xC0, 0x01); usleep(1000);   // HDMI mode
    IT66121_WRITE(0xC1, 0x00); usleep(1000);   // AV unmute
    IT66121_WRITE(0xC6, 0x03); usleep(1000);   // packet gen
    // 0x0F: 現状 0x00 なので bit4 は既に clear = TX clock 有効

    usleep(200 * 1000);
    IT66121_SNAP("post-fire");

    usleep(500 * 1000);
    IT66121_SNAP("+500ms   ");
    keep_running = 0;
    // 残りを回収
    struct timeval tv2 = {1, 0};
    for (int k = 0; k < 10 && inflight > 0; k++) libusb_handle_events_timeout(NULL, &tv2);

capture_cleanup:
    cleanup_iso_transfers(h, xfrs, bufs, devmem, NUM_TRANSFERS, transfer_bytes);
    if (outf) fclose(outf);
    hd60s_v4l2_close();
    hd60s_audio_close();
    fprintf(stderr, "\n=== HD60S Linux driver stats ===\n");
    fprintf(stderr, "iso pkts:  ok=%ld empty=%ld err=%ld (empty=%.2f%%)\n",
            pkt_ok, pkt_empty, pkt_err,
            (pkt_ok + pkt_empty) ? 100.0 * pkt_empty / (pkt_ok + pkt_empty) : 0.0);
    fprintf(stderr, "iso total: %lld bytes / %d s = %.1f Mbps\n",
            total_bytes, read_sec, total_bytes * 8.0 / read_sec / 1e6);
    fprintf(stderr, "parser:    frames_emitted=%llu resyncs=%llu (empty=%llu marker=%llu overflow=%llu)\n",
            g_frames_out, g_resyncs, g_resync_empty, g_resync_marker, g_resync_overflow);
    hd60s_audio_dump_stats(stderr);

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(NULL);
    return usb_session_fatal ? 2 : 0;
}
