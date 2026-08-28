#include <stdio.h>
#include <libusb-1.0/libusb.h>

int main(void) {
    libusb_context *ctx = NULL;
    int init_rc = libusb_init(&ctx);
    if (init_rc < 0) {
        fprintf(stderr, "libusb_init failed rc=%d (%s)\n",
                init_rc, libusb_error_name(init_rc));
        return 1;
    }
    libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x0fd9, 0x005e);
    if (!h) { fprintf(stderr, "device not found\n"); libusb_exit(ctx); return 2; }
    libusb_device *d = libusb_get_device(h);
    printf("speed=%d bus=%u addr=%u\n", libusb_get_device_speed(d),
           libusb_get_bus_number(d), libusb_get_device_address(d));
    struct libusb_config_descriptor *cfg = NULL;
    if (libusb_get_active_config_descriptor(d, &cfg) != 0) {
        int cfg_rc = libusb_get_config_descriptor(d, 0, &cfg);
        if (cfg_rc != 0) {
            fprintf(stderr, "get config descriptor failed rc=%d (%s)\n",
                    cfg_rc, libusb_error_name(cfg_rc));
            libusb_close(h);
            libusb_exit(ctx);
            return 3;
        }
    }
    const struct libusb_interface *iface = &cfg->interface[0];
    for (int a = 0; a < iface->num_altsetting; ++a) {
        const struct libusb_interface_descriptor *alt = &iface->altsetting[a];
        for (int e = 0; e < alt->bNumEndpoints; ++e) {
            const struct libusb_endpoint_descriptor *ep = &alt->endpoint[e];
            if ((ep->bEndpointAddress & 0x80) &&
                (ep->bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                int m = libusb_get_max_iso_packet_size(d, ep->bEndpointAddress);
                int max_packet = ep->wMaxPacketSize & 0x07ff;
                int transactions = ((ep->wMaxPacketSize >> 11) & 3) + 1;
                struct libusb_ss_endpoint_companion_descriptor *ss = NULL;
                libusb_get_ss_endpoint_companion_descriptor(NULL, ep, &ss);
                printf("alt=%u ep=0x%02x wMax=0x%04x max_packet=%d transactions=%d max_iso=%d interval=%u\n",
                       alt->bAlternateSetting, ep->bEndpointAddress,
                       ep->wMaxPacketSize, max_packet, transactions, m, ep->bInterval);
                if (ss) { printf("  ss burst=%u mult=%u bytes=%u\n", ss->bMaxBurst,
                                  ss->bmAttributes & 3, ss->wBytesPerInterval);
                          libusb_free_ss_endpoint_companion_descriptor(ss); }
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
