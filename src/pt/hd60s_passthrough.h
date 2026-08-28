#ifndef HD60S_PASSTHROUGH_H
#define HD60S_PASSTHROUGH_H

#include <libusb-1.0/libusb.h>

int hd60s_run_passthrough_only(libusb_device_handle *h, int argc, char **argv, int read_sec);

#endif
