#include "hd60s_passthrough.h"
#include "hd60s_replay.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int hd60s_run_passthrough_only(libusb_device_handle *h, int argc, char **argv, int read_sec) {
    // modo solo passthrough: no abre iso; dispara todas las escrituras de poststream-full al timing del pcap.
    // si el 6º arg es "release", tras poststream-full añade la «secuencia de liberación IT66121» (10 comandos).
    // secuencia de liberación = cola obligatoria que no está al final del pcap: AV_MUTE off + SW_RST all-clear + AFE fire + HDCP off.
    // (2026-07-09 de madrugada, hallado al mirar drivers públicos de IT66121: mainline it66121.c / HDZero / fl2000_drm)
    int do_release = (argc > 6 && strcmp(argv[6], "release") == 0);
    const char* pt_tsv = "analysis/poststream-full.tsv";
    if (argc > 6 && strcmp(argv[6], "nohdcp") == 0) {
        pt_tsv = "analysis/poststream-nohdcp.tsv";
        fprintf(stderr, "[pt-only] HDCP無効化モード\n");
    }
    fprintf(stderr, "[pt-only] %s を発火（iso 張らない）\n", pt_tsv);
    hd60s_load_burst(pt_tsv);
    hd60s_fire_burst(h);

    // diagnóstico: lee 0x0E SYS_STATUS de IT66121 (HPD/VID_STABLE)
    {
        unsigned char setup[3] = {0x9b, 0x01, 0x0e};
        unsigned char val = 0;
        libusb_control_transfer(h, 0x40, 192, 0x5066, 0, setup, 3, 500);
        libusb_control_transfer(h, 0xc0, 192, 0x5066, 0, &val, 1, 500);
        fprintf(stderr, "[diag] IT66121 SYS_STATUS(0x0E)=0x%02x HPD=%d VID_STABLE=%d\n",
                val, !!(val & 0x40), !!(val & 0x10));
    }

    // secuencia de liberación IT66121 (2026-07-09 de madrugada, drivers públicos ITE)
    // el pcap acaba con AV_MUTE ON+SW_RST aún puestos = Windows liberaba con comandos extra
    if (do_release) {
        fprintf(stderr, "[pt-only] IT66121 secuencia de liberación (AV_MUTE OFF, SW_RST, AFE, HDCP off)\n");
        static const unsigned char release_seq[][3] = {
            {0x9a, 0x0f, 0x00},  // selecciona bank 0 (por si acaso)
            {0x9a, 0x04, 0x00},  // SW_RST todo a 0 (VID/REF/AUD/AREF/HDCP)
            {0x9a, 0x62, 0x18},  // AFE_XP: RESETB=1, ENO=1
            {0x9a, 0x64, 0x94},  // AFE_IP: RESETB=1, GAINBIT=1, CKSEL_1 (>80MHz)
            {0x9a, 0x68, 0x00},  // LOWCLK clear
            {0x9a, 0x61, 0x00},  // AFE FIRE: RST=0 PWD=0 = arranca salida TMDS
            {0x9a, 0x20, 0x00},  // HDCP CPDESIRED=0 = no hace falta autenticar
            {0x9a, 0xc0, 0x01},  // modo HDMI (bit0=1)
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
        // re-diagnóstico
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
