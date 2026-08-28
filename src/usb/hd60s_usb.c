#include "hd60s_usb.h"
#include "hd60s_util.h"
#include "hd60s_parser.h"
#include "hd60s_pace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Hd60sUsbSession {
    libusb_device_handle *h;
    struct libusb_transfer **xfrs;
    unsigned char **bufs;
    unsigned char *devmem;
    int n;
    size_t buffer_bytes;
    FILE *outf;
    long long total_bytes;
    long pkt_ok, pkt_empty, pkt_err;
    int keep_running;
    int inflight;
    int max_inflight;
    unsigned long submit_ok, submit_fail, resubmit_fail;
    int usb_session_fatal;
    int usb_session_error;
} Hd60sUsbSession;

static Hd60sUsbSession *g_usb;

static Hd60sUsbSession *usb_session_get(void) {
    if (!g_usb) {
        g_usb = calloc(1, sizeof(*g_usb));
        if (g_usb)
            g_usb->keep_running = 1;
    }
    return g_usb;
}

int hd60s_usb_endpoint_capacity(libusb_device_handle* h, int alt,
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

static void LIBUSB_CALL iso_cb(struct libusb_transfer* xfr) {
    Hd60sUsbSession *s = xfr->user_data;
    if (!s) return;
    static unsigned long callback_count = 0;
    unsigned long long trace_cb_no = 0;
    if (g_trace_enabled) trace_cb_no = ++g_trace_callback_no;
    // HEX DUMP HOOK: HD60S_HEXDUMP=1 vuelca los primeros 32B de los primeros 500 iso packet
    // para distinguir paquetes de audio y de vídeo (workflow suggestion 2026-07-11)
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
                // alimentación en vivo al parser (hacia v4l2loopback)
                hd60s_parser_feed(buf, d->actual_length);
                // para verificación: solo se guarda el stream crudo de los primeros 512MB (aumentado para análisis de audio)
                if (s->total_bytes < (512LL << 20) && s->outf) fwrite(buf, 1, d->actual_length, s->outf);
                s->total_bytes += d->actual_length;
                if (g_diag) g_diag_input_bytes += d->actual_length;
                s->pkt_ok++;
                hd60s_parser_trace_end();
            } else {
                // un iso pkt de 0-length se considera pausa normal de blanking;
                // no se toca la posición de línea en curso (medido: resetear g_lpos en empty
                // aumenta el marker resync → lo correcto es no tocarlo).
                s->pkt_empty++;
                hd60s_parser_trace_end();
            }
        } else {
            s->pkt_err++;
            packet_loss = 1;
            lost_bytes += d->length;
            if (xfr->status == LIBUSB_TRANSFER_NO_DEVICE ||
                xfr->status == LIBUSB_TRANSFER_ERROR) {
                s->usb_session_fatal = 1;
                s->usb_session_error = xfr->status;
            }
            hd60s_parser_trace_end();
        }
    }
    if (packet_loss)
        hd60s_parser_notify_loss(lost_bytes);
    if (!g_stop_requested && s->keep_running && !s->usb_session_fatal) {
        int submit_rc = libusb_submit_transfer(xfr);
        if (submit_rc < 0) {
            s->inflight--;
            s->resubmit_fail++;
            s->usb_session_fatal = 1;
            s->usb_session_error = submit_rc;
            s->keep_running = 0;
            fprintf(stderr, "[iso] resubmit failed rc=%d (%s); ending USB session\n",
                    submit_rc, libusb_error_name(submit_rc));
        }
    } else {
        s->inflight--;
    }
    if ((++callback_count % 100) == 0)
        fprintf(stderr, "[iso-debug] callbacks=%lu ok=%ld empty=%ld err=%ld bytes=%lld frames=%llu\\n",
                callback_count, s->pkt_ok, s->pkt_empty, s->pkt_err, s->total_bytes, g_frames_out);
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
    Hd60sUsbSession *s = usb_session_get();
    if (!xfrs || !bufs) return;
    if (s) s->keep_running = 0;

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
    for (int wait = 0; wait < 100 && s && s->inflight > 0; wait++) {
        int rc = libusb_handle_events_timeout(NULL, &tv);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED && rc != LIBUSB_ERROR_NO_DEVICE)
            fprintf(stderr, "[iso] cancel-drain failed rc=%d (%s)\n",
                    rc, libusb_error_name(rc));
    }
    if (s && s->inflight > 0)
        fprintf(stderr, "[iso] warning: %d transfers remained after cancellation drain\n",
                s->inflight);
    if (s && s->inflight > 0) {
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

void hd60s_usb_open_dump(void) {
    Hd60sUsbSession *s = usb_session_get();
    if (!s) return;
    s->outf = fopen("captures/stream-iso.bin", "wb");
    if (!s->outf) fprintf(stderr, "生ストリーム出力ファイル開けず(続行)\n");
}

int hd60s_usb_start_iso(libusb_device_handle *h, int pkt_size, int n_pkts) {
    Hd60sUsbSession *s = usb_session_get();
    if (!s) return -1;
    s->h = h;
    s->n = NUM_TRANSFERS;
    s->buffer_bytes = (size_t)n_pkts * (size_t)pkt_size;
    if (s->buffer_bytes == 0 || s->buffer_bytes > (size_t)2147483647) {
        fprintf(stderr, "[iso] invalid transfer buffer size=%zu\n", s->buffer_bytes);
        s->usb_session_fatal = 1;
        s->usb_session_error = LIBUSB_ERROR_INVALID_PARAM;
        s->keep_running = 0;
        return -1;
    }
    s->xfrs = calloc(NUM_TRANSFERS, sizeof(*s->xfrs));
    s->bufs = calloc(NUM_TRANSFERS, sizeof(*s->bufs));
    s->devmem = calloc(NUM_TRANSFERS, sizeof(*s->devmem));
    if (!s->xfrs || !s->bufs || !s->devmem) {
        s->usb_session_fatal = 1;
        s->usb_session_error = LIBUSB_ERROR_NO_MEM;
        s->keep_running = 0;
        fprintf(stderr, "[iso] buffer allocation failed at transfer 0\n");
        return -1;
    }
    for (int i = 0; i < NUM_TRANSFERS; i++) {
        // buffer DMA zerocopy (vía usbfs mmap) → CPU↓, tail latency↓.
        // si falla, fallback a malloc (hosts pequeños donde KMS no puede reservar).
        s->bufs[i] = libusb_dev_mem_alloc(h, (int)s->buffer_bytes);
        if (s->bufs[i]) s->devmem[i] = 1;
        if (!s->bufs[i]) s->bufs[i] = malloc(s->buffer_bytes);
        if (!s->bufs[i]) {
            s->usb_session_fatal = 1;
            s->usb_session_error = LIBUSB_ERROR_NO_MEM;
            fprintf(stderr, "[iso] buffer allocation failed at transfer %d\n", i);
            break;
        }
        s->xfrs[i] = libusb_alloc_transfer(n_pkts);
        if (!s->xfrs[i]) {
            s->usb_session_fatal = 1;
            s->usb_session_error = LIBUSB_ERROR_NO_MEM;
            fprintf(stderr, "[iso] transfer allocation failed at transfer %d\n", i);
            break;
        }
        // timeout=0 = infinito. Con iso continuo un timeout finito cancela URB y vacía todos los in-flight packet: trampa
        libusb_fill_iso_transfer(s->xfrs[i], h, EP_STREAM, s->bufs[i],
            (int)s->buffer_bytes, n_pkts, iso_cb, s, 0);
        libusb_set_iso_packet_lengths(s->xfrs[i], pkt_size);
        int submit_rc = libusb_submit_transfer(s->xfrs[i]);
        if (submit_rc == 0) {
            s->inflight++;
            s->submit_ok++;
            if (s->inflight > s->max_inflight) s->max_inflight = s->inflight;
        } else {
            s->submit_fail++;
            fprintf(stderr, "submit%d failed rc=%d (%s)\\n", i, submit_rc, libusb_error_name(submit_rc));
        }
    }
    fprintf(stderr, "[main] iso転送 %d 本投入\n", s->inflight);

    fprintf(stderr, "[iso] submit summary ok=%lu fail=%lu max_inflight=%d current=%d\\n",
            s->submit_ok, s->submit_fail, s->max_inflight, s->inflight);
    if (s->inflight == 0 || s->usb_session_fatal) {
        s->usb_session_fatal = 1;
        if (!s->usb_session_error) s->usb_session_error = LIBUSB_ERROR_OTHER;
        s->keep_running = 0;
        fprintf(stderr, "[iso] no active transfers or allocation failure; ending USB session before capture\\n");
        return -1;
    }
    return 0;
}

int hd60s_usb_handle_events(struct timeval *tv) {
    Hd60sUsbSession *s = usb_session_get();
    int event_rc = libusb_handle_events_timeout(NULL, tv);
    if (event_rc == LIBUSB_ERROR_NO_DEVICE || event_rc == LIBUSB_ERROR_IO ||
        event_rc == LIBUSB_ERROR_OTHER) {
        if (s) {
            s->usb_session_fatal = 1;
            s->usb_session_error = event_rc;
            s->keep_running = 0;
        }
        fprintf(stderr, "[iso] event handling failed rc=%d (%s)\\n",
                event_rc, libusb_error_name(event_rc));
    }
    return event_rc;
}

void hd60s_usb_request_stop(void) {
    Hd60sUsbSession *s = usb_session_get();
    if (s) s->keep_running = 0;
}

void hd60s_usb_stop(libusb_device_handle *h) {
    Hd60sUsbSession *s = g_usb;
    if (!s) return;
    cleanup_iso_transfers(h, s->xfrs, s->bufs, s->devmem, s->n ? s->n : NUM_TRANSFERS, s->buffer_bytes);
    if (s->outf) {
        fclose(s->outf);
        s->outf = NULL;
    }
    free(s->xfrs);
    free(s->bufs);
    free(s->devmem);
    s->xfrs = NULL;
    s->bufs = NULL;
    s->devmem = NULL;
}

void hd60s_usb_note_ok(int nbytes) {
    Hd60sUsbSession *s = usb_session_get();
    if (!s) return;
    s->total_bytes += nbytes;
    s->pkt_ok++;
}

int hd60s_usb_keep_running(void) {
    Hd60sUsbSession *s = usb_session_get();
    return s ? s->keep_running : 0;
}
int hd60s_usb_inflight(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->inflight : 0;
}
int hd60s_usb_fatal(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->usb_session_fatal : 0;
}
int hd60s_usb_error(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->usb_session_error : 0;
}
long hd60s_usb_pkt_ok(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->pkt_ok : 0;
}
long hd60s_usb_pkt_empty(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->pkt_empty : 0;
}
long hd60s_usb_pkt_err(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->pkt_err : 0;
}
long long hd60s_usb_total_bytes(void) {
    Hd60sUsbSession *s = g_usb;
    return s ? s->total_bytes : 0;
}
