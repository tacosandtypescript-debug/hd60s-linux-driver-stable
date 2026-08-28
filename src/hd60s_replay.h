#ifndef HD60S_REPLAY_H
#define HD60S_REPLAY_H

#include <libusb-1.0/libusb.h>
#include <stdio.h>

typedef struct {
    double t;
    unsigned char brt, br;
    unsigned short wv, wi, wl;
    unsigned char data[80];
    int dlen, is_out;
} BurstCmd;

extern BurstCmd g_burst[2048];
extern int g_nburst;
extern FILE *g_rdlog;

void hd60s_replay_spell(libusb_device_handle *h, const char *path);
void hd60s_load_burst(const char *path);
void hd60s_fire_burst(libusb_device_handle *h);
int hd60s_wait_for_lock(libusb_device_handle *h, int timeout_ms, int poll_interval_ms);
int hd60s_apply_it6802_audio94(libusb_device_handle *h);
void hd60s_apply_post_iso_audio(libusb_device_handle *h);

#endif
