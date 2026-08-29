#include "hd60s_replay.h"
#include "hd60s_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

FILE *g_rdlog = NULL;

// replay de hechizo (TSV) ; retorno: (ok<<0) en la práctica se cuenta en globales
void hd60s_replay_spell(libusb_device_handle* h, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "TSV開けない: %s\n", path); return; }
    char line[8192];
    int ok=0, fail=0, first=1;
    unsigned char data[4096];
    double prev_t = -1;
    int n5066 = 0; char last5066[64] = "";
    int rd_idx = 0; char prev_out[64] = "";   // observación de diffs: payload OUT anterior
    while (fgets(line, sizeof(line), f)) {
        if (first) { first=0; continue; }              // header
        char c_frame[32], c_time[32], c_brt[16], c_br[16], c_wv[16], c_wi[16], c_wl[16], c_data[8000];
        c_data[0]=0;
        int nf = sscanf(line, "%31[^\t]\t%31[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%7999[^\t\n]",
                        c_frame, c_time, c_brt, c_br, c_wv, c_wi, c_wl, c_data);
        if (nf < 7) continue;
        // reproduce el timing de la captura original (sleep entre comandos). El intervalo 150ms de espera de lock HDMI también sale aquí.
        double t = atof(c_time);
        if (prev_t >= 0) {
            double dt = t - prev_t;
            if (dt > 0 && dt < 2.0) usleep((useconds_t)(dt * 1e6));   // tope 2s
        }
        prev_t = t;
        unsigned char brt = (unsigned char)strtol(c_brt, NULL, 16);
        unsigned char br  = (unsigned char)strtol(c_br, NULL, 10);
        unsigned short wv = (unsigned short)strtol(c_wv, NULL, 0);
        unsigned short wi = (unsigned short)strtol(c_wi, NULL, 0);
        unsigned short wl = (unsigned short)strtol(c_wl, NULL, 10);
        int is_out = (brt & 0x80) == 0;
        int r;
        if (is_out) {
            int dlen = (nf>=8 && c_data[0]) ? hex2bin(c_data, data, sizeof(data)) : 0;
            r = libusb_control_transfer(h, brt, br, wv, wi, data, dlen, 1000);
            // observación de diffs: registra este OUT (=setup I2C). Se ata a la siguiente lectura IN.
            if (nf>=8 && c_data[0]) { strncpy(prev_out, c_data, sizeof(prev_out)-1); prev_out[sizeof(prev_out)-1]=0; }
        } else {
            r = libusb_control_transfer(h, brt, br, wv, wi, data, wl, 1000);
            // log de diffs: todos los IN como «posición idx / wValue / OUT anterior (objetivo I2C) / respuesta».
            if (g_rdlog) {
                char rh[64]; int p=0;
                for (int j=0;j<r && j<16;j++) p+=sprintf(rh+p,"%02x",data[j]);
                if (r<=0) { rh[0]='-'; rh[1]=0; }
                fprintf(g_rdlog, "%d\t0x%04x\t%s\t%s\n", rd_idx, wv, prev_out[0]?prev_out:"-", rh);
            }
            rd_idx++;
            // diagnóstico: registra respuestas del poll de status (wV=0x5066) y mira cambios (=detección de lock)
            if (r > 0 && wv == 0x5066 && n5066 < 4000) {
                char hex[64]; int p=0;
                for (int j=0;j<r && j<16;j++) p+=sprintf(hex+p,"%02x",data[j]);
                // muestra solo respuestas distintas a la última (para pillar el punto de cambio)
                if (strcmp(hex, last5066) != 0) {
                    fprintf(stderr, "  [poll@%.2fs] wV5066 resp: %s\n", t, hex);
                    snprintf(last5066, sizeof(last5066), "%s", hex);
                }
                n5066++;
            }
        }
        if (r < 0) fail++; else ok++;
    }
    fclose(f);
    fprintf(stderr, "[replay] 呪文再生: ok=%d fail=%d / wV5066ポーリング読み %d回\n", ok, fail, n5066);
}

// espera del bit candidato SCDT (lock de señal): bit0x80 de reg 0x12 del bank `9d`
// hasta que sea 0 (=hay señal, medido ON=0x11).
// Orden de burst medido: con HDMI vs sin HDMI (FINDINGS interno).
int hd60s_wait_for_lock(libusb_device_handle* h, int timeout_ms, int poll_interval_ms) {
    unsigned char setup[3] = {0x9d, 0x01, 0x12};
    unsigned char resp[4];
    int waited = 0;
    while (waited < timeout_ms) {
        int r1 = libusb_control_transfer(h, 0x40, 192, 0x5066, 0, setup, 3, 500);
        int r2 = libusb_control_transfer(h, 0xc0, 192, 0x5066, 0, resp, 1, 500);
        if (r1 >= 0 && r2 >= 1) {
            fprintf(stderr, "[lock] 9d:0x12 = 0x%02x (%s)\n", resp[0],
                    (resp[0] & 0x80) ? "信号なし" : "ロック済み!");
            if (!(resp[0] & 0x80)) return 1;   // bit7 clear = lock
        } else {
            fprintf(stderr, "[lock] poll falló r1=%d r2=%d\n", r1, r2);
        }
        struct timeval tv = {0, poll_interval_ms * 1000};
        libusb_handle_events_timeout(NULL, &tv);
        waited += poll_interval_ms;
    }
    return 0; // timeout
}

// --- burst de ignición post-stream (frame8637-13573) ---
// comandos que Windows manda *después* de abrir iso en alt2 para habilitar el pipe de vídeo.
// esto es lo que realmente arranca el formateador IT6802E + DMA FX3 (hallado en workflow 2026-07-09).
BurstCmd g_burst[2048];
int g_nburst = 0;

void hd60s_load_burst(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[burst] tsv開けない: %s\n", path); return; }
    char line[8192]; int first = 1;
    while (fgets(line, sizeof(line), f) && g_nburst < 2048) {
        if (first) { first = 0; continue; }
        char c_frame[32], c_time[32], c_brt[16], c_br[16], c_wv[16], c_wi[16], c_wl[16], c_data[8000];
        c_data[0] = 0;
        int nf = sscanf(line, "%31[^\t]\t%31[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%15[^\t]\t%7999[^\t\n]",
                        c_frame, c_time, c_brt, c_br, c_wv, c_wi, c_wl, c_data);
        if (nf < 7) continue;
        BurstCmd* b = &g_burst[g_nburst];
        b->t = atof(c_time);
        b->brt = (unsigned char)strtol(c_brt, NULL, 16);
        b->br  = (unsigned char)strtol(c_br, NULL, 10);
        b->wv  = (unsigned short)strtol(c_wv, NULL, 0);
        b->wi  = (unsigned short)strtol(c_wi, NULL, 0);
        b->wl  = (unsigned short)strtol(c_wl, NULL, 10);
        b->is_out = (b->brt & 0x80) == 0;
        b->dlen = (nf >= 8 && c_data[0]) ? hex2bin(c_data, b->data, sizeof(b->data)) : 0;
        g_nburst++;
    }
    fclose(f);
    fprintf(stderr, "[burst] %d コマンド ロード\n", g_nburst);
}

int hd60s_apply_it6802_audio94(libusb_device_handle* h) {
    #include "post_iso_audio94.inc"
    int ok94 = 0;
    fprintf(stderr, "[audio-init] applying IT6802E bank 0x94 audio configuration (%d writes)...\n",
            post_iso_audio94_n);
    for (int u = 0; u < post_iso_audio94_n; u++) {
        unsigned char w[3] = {post_iso_audio94[u].b0,
                              post_iso_audio94[u].b1,
                              post_iso_audio94[u].b2};
        int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 200);
        if (r == 3) ok94++;
        usleep(600);
    }
    fprintf(stderr, "[audio-init] IT6802E bank 0x94 audio config: %d/%d OK\n", ok94, post_iso_audio94_n);
    return ok94;
}


void hd60s_fire_burst(libusb_device_handle* h) {
    double t0 = g_nburst ? g_burst[0].t : 0;
    double start = now_s();
    int ok = 0, fail = 0;
    for (int i = 0; i < g_nburst; i++) {
        BurstCmd* b = &g_burst[i];
        double target = b->t - t0;
        double now = now_s() - start;
        if (target > now) usleep((useconds_t)((target - now) * 1e6));
        unsigned char inbuf[80]; int r;
        if (b->is_out) r = libusb_control_transfer(h, b->brt, b->br, b->wv, b->wi, b->data, b->dlen, 1000);
        else r = libusb_control_transfer(h, b->brt, b->br, b->wv, b->wi, inbuf, b->wl, 1000);
        if (r < 0) fail++; else ok++;
    }
    fprintf(stderr, "[pt-only] 発火完了 ok=%d fail=%d elapsed=%.2fs\n", ok, fail, now_s() - start);
}

void hd60s_apply_post_iso_audio(libusb_device_handle* h) {
    #include "post_iso_audio.inc"
    fprintf(stderr, "[post-iso-audio] firing %d I2C writes...\n", post_iso_audio_n);
    int ok_pia = 0;
    for (int u = 0; u < post_iso_audio_n; u++) {
        if (post_iso_audio[u].delay_us > 0) usleep(post_iso_audio[u].delay_us);
        unsigned char w[3] = {post_iso_audio[u].b0, post_iso_audio[u].b1, post_iso_audio[u].b2};
        int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 200);
        if (r == 3) ok_pia++;
    }
    fprintf(stderr, "[post-iso-audio] fired %d/%d OK\n", ok_pia, post_iso_audio_n);
}
