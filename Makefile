CC ?= gcc
CFLAGS ?= -O2 -Wall
PREFIX ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec/hd60s
UDEVRULEDIR ?= /etc/udev/rules.d
UDEVRULE := 70-elgato-hd60s.rules
SYSTEMD_USER_UNITDIR ?= $(PREFIX)/share/systemd/user
SYSTEMD_USER_UNIT_TEMPLATE := hd60s.service.in
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0)
LIBUSB_LIBS := $(shell pkg-config --libs libusb-1.0)
ALSA_LIBS := -lasound
PIPEWIRE_CFLAGS := $(shell pkg-config --cflags libpipewire-0.3)
PIPEWIRE_LIBS := $(shell pkg-config --libs libpipewire-0.3)

BINS = iso_capture audio_extract offline_parser spi_dump probe_iso

all: $(BINS)

iso_capture: iso_capture.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) $(PIPEWIRE_CFLAGS) $< -o $@ $(LIBUSB_LIBS) $(ALSA_LIBS) $(PIPEWIRE_LIBS) -lpthread -lsamplerate

audio_extract: audio_extract.c
	$(CC) $(CFLAGS) $< -o $@

offline_parser: offline_parser.c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(BINS)

install-symlink:
	ln -sf $(CURDIR)/hd60s /usr/local/bin/hd60s

.PHONY: all clean install install-symlink uninstall

install: all
	install -d "$(DESTDIR)$(LIBEXECDIR)/analysis" "$(DESTDIR)$(PREFIX)/bin" "$(DESTDIR)$(SYSTEMD_USER_UNITDIR)"
	install -m 0755 iso_capture probe_iso hd60s run-hd60s-obs.sh "$(DESTDIR)$(LIBEXECDIR)/"
	install -m 0644 analysis/*.tsv "$(DESTDIR)$(LIBEXECDIR)/analysis/"
	@sed -e 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' "$(SYSTEMD_USER_UNIT_TEMPLATE)" > "$(DESTDIR)$(SYSTEMD_USER_UNITDIR)/hd60s.service"
	install -d "$(DESTDIR)$(UDEVRULEDIR)"
	install -m 0644 $(UDEVRULE) "$(DESTDIR)$(UDEVRULEDIR)/$(UDEVRULE)"
	ln -sfn "$(LIBEXECDIR)/hd60s" "$(DESTDIR)$(PREFIX)/bin/hd60s"
	@if test -z "$(DESTDIR)" && command -v udevadm >/dev/null 2>&1; then \
		udevadm control --reload-rules; udevadm trigger --subsystem-match=usb; \
	fi

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/hd60s"
	rm -f "$(DESTDIR)$(UDEVRULEDIR)/$(UDEVRULE)"
	rm -f "$(DESTDIR)$(SYSTEMD_USER_UNITDIR)/hd60s.service"
	rm -rf "$(DESTDIR)$(LIBEXECDIR)"

spi_dump: spi_dump.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) $< -o $@ $(LIBUSB_LIBS)

probe_iso: probe_iso.c
	$(CC) $(CFLAGS) $(LIBUSB_CFLAGS) $< -o $@ $(LIBUSB_LIBS)
