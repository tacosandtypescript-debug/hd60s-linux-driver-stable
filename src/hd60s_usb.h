#ifndef HD60S_USB_H
#define HD60S_USB_H

#include <libusb-1.0/libusb.h>
#include <sys/time.h>

#define VID 0x0fd9
#define PID 0x005e
#define EP_STREAM 0x83
#define NUM_TRANSFERS 506

int hd60s_usb_endpoint_capacity(libusb_device_handle *h, int alt,
                                int *capacity, int *superspeed);
void hd60s_usb_open_dump(void);
int hd60s_usb_start_iso(libusb_device_handle *h, int pkt_size, int n_pkts);
int hd60s_usb_handle_events(struct timeval *tv);
void hd60s_usb_stop(libusb_device_handle *h);
void hd60s_usb_request_stop(void);
void hd60s_usb_note_ok(int nbytes);
int hd60s_usb_keep_running(void);
int hd60s_usb_inflight(void);
int hd60s_usb_fatal(void);
int hd60s_usb_error(void);
long hd60s_usb_pkt_ok(void);
long hd60s_usb_pkt_empty(void);
long hd60s_usb_pkt_err(void);
long long hd60s_usb_total_bytes(void);

#endif
