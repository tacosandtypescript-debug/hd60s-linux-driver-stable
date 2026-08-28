#ifndef HD60S_UNMUTE_H
#define HD60S_UNMUTE_H

#include <libusb-1.0/libusb.h>

void hd60s_unmute_maybe_audio94(libusb_device_handle *h);
void hd60s_unmute_oneshot(libusb_device_handle *h);
void hd60s_unmute_maybe_post_iso_audio(libusb_device_handle *h);

#endif
