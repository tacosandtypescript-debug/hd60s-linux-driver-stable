// HD60 S: replay de hechizo + captura iso(alt2) (libusb-1.0, C)
// node-usb no soporta iso, así que está en C. Misma ruta iso que Windows: chupa vídeo (YUYV crudo) de EP0x83.
//
// build: gcc -O2 -Isrc src/iso_capture.c src/hd60s_util.c src/hd60s_v4l2.c src/hd60s_audio.c src/hd60s_replay.c src/hd60s_pace.c src/hd60s_parser.c src/hd60s_usb.c -o iso_capture $(pkg-config --libs --cflags libusb-1.0)
// run  : sudo ./iso_capture [readSec=6] [alt=2] > /dev/null  (el vídeo va a captures/stream-iso.bin)
//
// ======================================================================
// TODO (trabajo pendiente):
//   - Ruta de audio vía MCU: ahora se extrae audio del SEP payload 8B, pero
//     según la fuente sigue en silencio. La secuencia unmute de IT6802E/IT66121 (HD60S_UNMUTE)
//     y su combinación con comandos MCU 0x509c aún no está fijada.
//   - Passthrough (loop-through a HDMI OUT): en modo pt-only se mantiene
//     de forma temporal, pero la secuencia permanente de liberación AV_MUTE/SW_RST/AFE de IT66121
//     no está claro si es responsabilidad del host o del MCU. Se busca una ruta
//     "fire-and-forget" que evite el keepalive-cycle.
// ======================================================================
//
// ======================================================================
// SECTION 0: cabeceras / constantes / estado global
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
#include "hd60s_usb.h"
#include "hd60s_passthrough.h"
#include "hd60s_unmute.h"

#define MAX_WRITE_BYTES (64LL << 20)  // el fichero de salida se corta a 64MB (evitar hinchazón a 2Gbps)

// ======================================================================
// SECTION 4: main (open del dispositivo → replay de hechizo → captura iso → estadísticas)
// ======================================================================
// TODO: activar passthrough (HDMI loop-through) de forma permanente y, a la vez,
// capturar. Aún no resuelto; hoy pt-only solo lo mantiene un rato.
int main(int argc, char** argv) {
    hd60s_diag_set(getenv("HD60S_CADENCE_DIAG") ? 1 : 0);
    const char *pace_env = getenv("HD60S_PACE_OUTPUT");
    hd60s_pace_configure(pace_env && pace_env[0] && pace_env[0] != '0' &&
                    pace_env[0] != 'n' && pace_env[0] != 'N');
    if (g_diag) fprintf(stderr, "[cadence] diagnostics enabled (no capture parameters changed)\n");
    if (g_pace_output) fprintf(stderr, "[cadence] V4L2 output pacing enabled at 60 Hz\n");
    // stderr sin buffer (con prioridad RT SCHED_FIFO el buffer por bloques
    // retrasa los logs; esto lo evita)
    setvbuf(stderr, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    hd60s_parser_trace_init();
    install_signal_handlers();
    int read_sec = argc > 1 ? atoi(argv[1]) : 6;
    // 0 o negativo se trata como «infinito práctico» (~68 años)
    // nota: 100 años = 3,153,600,000 desborda int; INT_MAX-3600 es seguro
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
    // si el 5º arg es "pt": modo solo passthrough (sin iso; solo dispara burst 9a y mantiene)
    int passthrough_only = (argc > 5 && strcmp(argv[5], "pt") == 0);
    // la etiqueta es el 5º (en modo pt, el 6º)
    if (passthrough_only && argc > 6) argv[4] = argv[6];

    // 2026-07-10 se retira SCHED_FIFO: mejora de stutter medida = cero, y además
    // este proceso monopoliza la CPU y el trabajo interno de libusb (otro hilo/worker del kernel)
    // no se agenda: replay_spell se cuelga tras "open/claim OK"
    // efecto grave confirmado (reproducido; CPU 0% y parado). Se deja solo mlockall (sin daño).
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
    // 2026-07-10 reset forzado del dispositivo: tras un crash o un unplug el bus I2C se bloquea
    // o el estado interno se corrompe (medido: sin reset, 9d:0x12 devuelve 0x9d y 100% paquetes vacíos)
    // HD60S_NO_RESET=1 para saltarlo (cuando no se quiere romper el passthrough)
    const char* env_no_reset = getenv("HD60S_NO_RESET");
    int no_reset = (env_no_reset && env_no_reset[0] && env_no_reset[0] != '0' && env_no_reset[0] != 'n' && env_no_reset[0] != 'N');
    int rst = no_reset ? 0 : libusb_reset_device(h);
    if (no_reset) fprintf(stderr, "[main] reset 省略 (HD60S_NO_RESET)\n");
    if (rst == LIBUSB_ERROR_NOT_FOUND) {
        // tras el reset el ID del dispositivo a veces cambia → reabrir
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
        int endpoint_rc = hd60s_usb_endpoint_capacity(h, alt, &endpoint_capacity,
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

    // log de observación de diffs: el 5º arg es etiqueta; todos los IN van a captures/reads-<label>.tsv
    const char* label = argc > 4 ? argv[4] : NULL;
    if (label) {
        char lp[256]; snprintf(lp, sizeof(lp), "captures/reads-%s.tsv", label);
        g_rdlog = fopen(lp, "w");
        if (g_rdlog) { fprintf(g_rdlog, "idx\twValue\tsetupOUT\tresponse\n"); fprintf(stderr, "[main] 読みログ: %s\n", lp); }
    }

    // HD60S_SKIP_INIT=1 omite por completo el replay del hechizo init + burst
    // (hipótesis: si HD60S ya está init en otra sesión, tocarlo rompe el passthrough)
    const char* env_skip_init = getenv("HD60S_SKIP_INIT");
    int skip_init = (env_skip_init && env_skip_init[0] && env_skip_init[0] != '0' && env_skip_init[0] != 'n' && env_skip_init[0] != 'N');
    if (!skip_init) {
        // la env HD60S_INIT_TSV permite cambiar el path del TSV de init
        const char* init_tsv = getenv("HD60S_INIT_TSV");
        if (!init_tsv || !*init_tsv) init_tsv = "analysis/init-p2-audio-fast.tsv";
        fprintf(stderr, "[main] init: %s\n", init_tsv);
        hd60s_replay_spell(h, init_tsv);
        if (g_rdlog) { fclose(g_rdlog); g_rdlog = NULL; }
    } else {
        fprintf(stderr, "[main] HD60S_SKIP_INIT=1: init replay 省略\n");
    }

    // espera el lock de señal HDMI «mirándolo de verdad» (2026-07-09: 9d:0x12 bit0x80).
    // si el arg 3 es negativo, fallback a la espera fija de siempre (para comparar).
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

    // diagnóstico (rank2): lee registros de timing de entrada y comprueba resolución (bank 9d). Esperado=1920x1080@60.
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

    if (passthrough_only)
        return hd60s_run_passthrough_only(h, argc, argv, read_sec);

    // carga el burst de ignición (se emite con timing relativo tras arrancar iso)
    // HD60S_SKIP_BURST=1 omite todo el burst poststream (protege passthrough)
    const char* env_skip_burst = getenv("HD60S_SKIP_BURST");
    int skip_burst = skip_init || (env_skip_burst && env_skip_burst[0] && env_skip_burst[0] != '0' && env_skip_burst[0] != 'n' && env_skip_burst[0] != 'N');
    if (!skip_burst) {
        const char* burst_tsv = getenv("HD60S_BURST_TSV");
        if (!burst_tsv || !*burst_tsv) burst_tsv = "analysis/poststream-no9a.tsv";
        hd60s_load_burst(burst_tsv);
    } else {
        fprintf(stderr, "[main] burst load 省略\n");
    }

    // 🔥 PASSTHROUGH PRE-ISO ENABLE (2026-07-11 experimento Fable + kusq webcam)
    // El 1er enable trio del pcap de Windows se dispara **384ms antes de arrancar iso**.
    // O sea: tras el TSV de init, antes de alt=2 = hay que dispararlo aquí. Siempre activo.
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
        // (3) 5098 data=[0x20, 0x05] LE (también visto en Windows)
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
                hd60s_usb_note_ok(actual);
                if (g_diag) g_diag_input_bytes += actual;
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
                hd60s_usb_total_bytes(), g_frames_out, g_resyncs);
        libusb_release_interface(h, 0);
        libusb_close(h);
        libusb_exit(NULL);
        return nonempty ? 0 : 4;
    }
    // mitigación passthrough (H1): 103ms medidos en pcap entre SET_INTERFACE y el 1er burst (evita choque GPIF/I2C al reiniciar FX3)
    { struct timeval td={0, 120000}; libusb_handle_events_timeout(NULL, &td); }
    fprintf(stderr, "[main] iso開始 %d秒\n", read_sec);

    // IT6802E bank 0x94 audio setup: 0x0e=0x8b, 0x0f=0x8b, 0x10=0x05, 0x11=0x05, etc.
    // Must be applied before audio_open() and before submitting ISO transfers.
    hd60s_unmute_maybe_audio94(h);

    // abre v4l2loopback (/dev/video42). Si falla, sigue (solo guarda el stream crudo).
    hd60s_v4l2_open("/dev/video42");
    // abre ALSA snd-aloop hw:10,0. Env HD60S_ALSA_DEV para elegir el dispositivo.
    const char* alsa_dev = getenv("HD60S_ALSA_DEV");
    if (!alsa_dev) alsa_dev = "hw:10,0";
    hd60s_audio_open(alsa_dev);

    hd60s_usb_open_dump();

    // replay de la audio init sequence 0x509c (MCU bridge) según el análisis del firmware
    // 236 writes de 2 bytes extraídos del pcap init-timed t=6.5-7.2s
    // se activa con env HD60S_509C=1
    // env = "1"/"yes"/"on" para activar (si solo se mira getenv()!=NULL, "0" también queda ON)
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
            usleep(500);  // intervalo 500us medido en pcap
        }
        fprintf(stderr, "[509c-init] %d/%d ok\n", ok, seq_len);
    }

    hd60s_unmute_oneshot(h);


    if (hd60s_usb_start_iso(h, iso_pkt_size, iso_packets) < 0) {
        goto capture_cleanup;
    }

    // 🔍 dump de estado IT66121 (para separar antes/después de la sequence de passthrough)
    // la macro está definida más abajo en el file, así que se escribe inline
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

    // 🔥 HDMI PASSTHROUGH ENABLE (2026-07-11 análisis Fable 3rd)
    // culpable real: el TX IT66121 (slave 0x9a) ya estaba perfecto. El verdadero enable de passthrough
    // son «6 comandos secretos» vía MCU (slave 0xaa magic 12 34) y CPLD (slave 0xd4).
    // Windows los dispara 3 veces. Abren «reg 0x27 = video gate» y habilitan la ruta RX→TX
    // en el CPLD.
    // la versión anterior (15 writes to 0x9a) solo tocaba el TX, así que el camino físico RX→TX
    // seguía cortado en el CPLD = TV sin señal.
    {
        int pt_ok = 0, pt_fail = 0;
        #define TX_WRITE(reg, val) do { \
            unsigned char _w[3] = {0x9a, (reg), (val)}; \
            int _r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, _w, 3, 500); \
            if (_r == 3) pt_ok++; else pt_fail++; \
        } while (0)

        // 🎯 secuencia real de enable de passthrough (análisis Fable 3rd)
        // Windows dispara 3 veces, aquí también 3
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
            // 4. CPLD routing reg 0x04 = 0x03 (bit0=RX→TX, bit1=RX→FX3, ambos enable)
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
            usleep(500 * 1000);  // 3 veces, cada 500ms
        }
        fprintf(stderr, "[passthrough] MCU/CPLD enable 統計: ok=%d fail=%d\n", pt_ok, pt_fail);
        pt_ok = 0; pt_fail = 0;

        // 🔥 se eliminan todos los TX writes a 0x9a (2026-07-11 análisis Fable 4th)
        // HD60S trae passthrough ON por defecto; el MCU controla IT66121 solo.
        // escribir 0x9a desde el host rompe la config del MCU (friendly fire) → TMDS inestable →
        // la TV cae a «modo ahorro de energía». Solo queda el control MCU/CPLD (aa/d4);
        // no se escribe nada a 0x9a.
        (void)pt_ok; (void)pt_fail;
        #undef TX_WRITE
    }
    // 🔍 dump de estado IT66121 (justo después de la sequence de passthrough)
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
    hd60s_unmute_maybe_post_iso_audio(h);
    // IT6802E bank 0x94 audio setup (also applied before audio stream open).
    hd60s_unmute_maybe_audio94(h);

    // mientras se bombea iso, se emite el burst de ignición según timestamps relativos.
    // un solo hilo: libusb_control_transfer también bombea iso por dentro, así que iso no se para.
    double burst_t0 = g_nburst ? g_burst[0].t : 0;
    double start = now_s();
    int bi = 0, bok = 0, bfail = 0;
    struct timeval tv = {0, g_pace_output ? 1000 : 10000}; // paced: 1ms wakeup
    const char* env_pt = getenv("HD60S_PT_LOOP");
    int pt_loop = (env_pt && env_pt[0] && env_pt[0] != '0' && env_pt[0] != 'n' && env_pt[0] != 'N');
    double last_pt_fire = 0.0;
    int pt_fires = 0;
    while (!g_stop_requested && hd60s_usb_keep_running() && now_s() - start < read_sec && hd60s_usb_inflight() > 0) {
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
        // AUDIO KEEPALIVE V2: 2026-07-11 hallazgo del subagente Opus 4.8.
        // El steady-state de Windows dispara un keepalive cycle de 227 comandos cada 163ms.
        // se activa con env HD60S_AUDIO_KA=1. Carga el TSV de keepalive en otro buffer y reenvía.
        static double last_ka_fire = 0.0;
        static int ka_fires = 0;
        static BurstCmd g_ka[512];
        static int g_nka = -1;  // -1 = aún no cargado
        const char* env_ka = getenv("HD60S_AUDIO_KA");
        int ka_loop = (env_ka && env_ka[0] && env_ka[0] != '0');
        if (ka_loop && g_nka < 0) {
            // primera vez: TSV load (querría otra función, pero load_burst pisa g_burst → inline)
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
        // reproduce AudioFsCal() + aud_fiforst() + Force FS del driver oficial ITE it680x.c.
        // cada 100ms reenvía HW unmute + forzar 48kHz + I2S untri-state.
        // se activa con HD60S_IT6802_RECOVER=1. Acceso IT6802 = I2C slave 0x94 (write) bank 0
        static double last_it6802_rec = 0.0;
        static int it6802_rec_fires = 0;
        const char* env_rec = getenv("HD60S_IT6802_RECOVER");
        int do_rec = (env_rec && env_rec[0] && env_rec[0] != '0' && env_rec[0] != 'n' && env_rec[0] != 'N');
        static double rec_interval = -1.0;
        if (rec_interval < 0) {
            const char* env_int = getenv("HD60S_RECOVER_MS");
            rec_interval = (env_int && atoi(env_int) > 0) ? atoi(env_int) / 1000.0 : 0.100;
        }
        // también dispara en t=0 (primera vez last_it6802_rec == 0.0; mata el silencio inicial)
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

        // 🔥 PASSTHROUGH KEEPALIVE (2026-07-11 verificado Fable + kusq webcam)
        // si no se dispara el keepalive del slave d4 MCU/CPLD cada 100ms, el monitor LG
        // cae a «modo ahorro de energía» (detecta el corte passthrough ON→OFF).
        // En el pcap de Windows se dispara en continuo cada 40-300ms.
        static double last_pt_ka = 0.0;
        static int pt_ka_fires = 0;
        // In paced capture this synchronous five-transfer maintenance cycle
        // blocks the same thread that must submit V4L2 frames at 60 Hz.  The
        // passthrough keepalive is retained for unpaced/pass-through runs,
        // but must not stall the capture presentation clock.
        if (!g_pace_output && (el - last_pt_ka) >= 0.100) {
            // dispara enable trio + keepalive pair cada vez
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

        // 60B MCU BATCH RETRY: durante iso, re-dispara el arm batch para intentar restart del DMA de audio
        // se activa con env HD60S_BATCH_LOOP=1, periodo 80ms
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

        // PLL LOCK MONITOR: 2026-07-11 pista de Fable. Monitoriza cada 20ms si IPLL_LOCK
        // de IT6802 cae cuando muere el audio.
        // se activa con env HD60S_PLL_MON=1
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

        // AUDIO ARM RETRY: 2026-07-11 reinyecta la arm sequence durante iso.
        // el audio muere a los 100ms → ¿el efecto del arm solo dura 100ms?
        // se activa con env HD60S_ARM_LOOP=1, periodo 30ms
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

        // AUDIO UNMUTE-RETRY: 2026-07-11 pista de Fable. IT6802E hace hard-mute al ACR unlock
        // → hace falta read-then-clear. Cada 50ms, write forzado de mute clear.
        // se activa con env HD60S_UNMUTE_RETRY=1
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

        // min-unmute-loop: tras el burst, reenvía bank2 select + reg 0x20=0x00 cada 100ms
        // se activa con env HD60S_MIN_LOOP=1 (en P5 se confirmó peak +71%)
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

        // pt-loop: tras el burst, dispara el keep-alive cycle **completo** de passthrough cada 120ms
        // análisis pcap: 30 commands (writes 9a, reads 9b/9d, MCU 0x509c) todos cada 120ms
        // detalle en analysis/keepalive-cycle.tsv
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
    hd60s_usb_handle_events(&tv);
        hd60s_pace_output_if_due();
    }
    if (g_stop_requested) {
        hd60s_usb_request_stop();
        fprintf(stderr, "[main] stop requested; draining USB transfers\n");
    }
    if (hd60s_usb_fatal())
        fprintf(stderr, "[iso] USB session ended: status=%d (%s)\n",
                hd60s_usb_error(),
                hd60s_usb_error() < 0 ? libusb_error_name(hd60s_usb_error()) : "transfer status");
    if (hd60s_usb_fatal())
        goto capture_cleanup;
    if (pt_loop) fprintf(stderr, "[pt-loop] 継続発火 %d 回\n", pt_fires);
    fprintf(stderr, "[burst] 発行 %d/%d (ok=%d fail=%d)\n", bi, g_nburst, bok, bfail);

    // The pre-burst endpoint carries control/blanking data that can contain
    // line-marker byte patterns.  Any parser lock obtained there is invalid
    // for the video phase.  Start a fresh sliding-window search immediately
    // after the stream-enable burst has completed.
    hd60s_parser_reset_video_phase();
    fprintf(stderr, "[parser] video-phase synchronization reset after burst\n");

    // (pt-loop corre inline en el main iso loop; se quitó el re-loop de aquí)

    // función de lectura de SYS_STATUS de IT66121 (para comparar antes/después del arm)
    // reg 0x0E es SYS_STATUS. En docs ITE, bit4=VID_STABLE
    #define IT66121_READ(reg) ({ \
        unsigned char _setup[3] = {0x9b, 0x01, (unsigned char)(reg)}; \
        libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, _setup, 3, 1000); \
        unsigned char _resp[8]; \
        int _rr = libusb_control_transfer(h, 0xC0, 0xC0, 0x5066, 0, _resp, 1, 1000); \
        (_rr > 0) ? _resp[0] : 0xff; \
    })

    // snapshot de estado pre-arm (i2c read de varios register; 5ms entre lecturas)
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

    // propuesta Workflow synth: reenviar explícitamente el batch MCU arm 60B (payload del frame 13573)
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

    // intento extra: enviar explícitamente el procedimiento "FireAFE + HDMI mode + unmute" del driver IT66121 (Linux mainline)
    // de ite-it66121.c:
    //   0x61 = 0x00 (FireAFE: arranca AFE)
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
    // 0x0F: ahora es 0x00, así que bit4 ya está clear = TX clock activo

    usleep(200 * 1000);
    IT66121_SNAP("post-fire");

    usleep(500 * 1000);
    IT66121_SNAP("+500ms   ");
    hd60s_usb_request_stop();
    // recoger el resto
    struct timeval tv2 = {1, 0};
    for (int k = 0; k < 10 && hd60s_usb_inflight() > 0; k++) libusb_handle_events_timeout(NULL, &tv2);

capture_cleanup:
    hd60s_usb_stop(h);
    hd60s_v4l2_close();
    hd60s_audio_close();
    fprintf(stderr, "\n=== HD60S Linux driver stats ===\n");
    fprintf(stderr, "iso pkts:  ok=%ld empty=%ld err=%ld (empty=%.2f%%)\n",
            hd60s_usb_pkt_ok(), hd60s_usb_pkt_empty(), hd60s_usb_pkt_err(),
            (hd60s_usb_pkt_ok() + hd60s_usb_pkt_empty()) ? 100.0 * hd60s_usb_pkt_empty() / (hd60s_usb_pkt_ok() + hd60s_usb_pkt_empty()) : 0.0);
    fprintf(stderr, "iso total: %lld bytes / %d s = %.1f Mbps\n",
            hd60s_usb_total_bytes(), read_sec, hd60s_usb_total_bytes() * 8.0 / read_sec / 1e6);
    fprintf(stderr, "parser:    frames_emitted=%llu resyncs=%llu (empty=%llu marker=%llu overflow=%llu)\n",
            g_frames_out, g_resyncs, g_resync_empty, g_resync_marker, g_resync_overflow);
    hd60s_audio_dump_stats(stderr);

    libusb_release_interface(h, 0);
    libusb_close(h);
    libusb_exit(NULL);
    return hd60s_usb_fatal() ? 2 : 0;
}
