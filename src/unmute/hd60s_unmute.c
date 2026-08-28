#include "hd60s_unmute.h"
#include "hd60s_replay.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void hd60s_unmute_maybe_audio94(libusb_device_handle *h) {
    const char* env_pia94 = getenv("HD60S_POST_ISO_AUDIO94");
    int do_pia94 = !(env_pia94 && (env_pia94[0] == '0' || env_pia94[0] == 'n' || env_pia94[0] == 'N'));
    if (do_pia94) {
        hd60s_apply_it6802_audio94(h);
    }
}

void hd60s_unmute_maybe_post_iso_audio(libusb_device_handle *h) {
    const char* env_pia = getenv("HD60S_POST_ISO_AUDIO");
    int do_pia = (env_pia && env_pia[0] && env_pia[0] != '0' && env_pia[0] != 'n' && env_pia[0] != 'N');
    if (do_pia) {
        hd60s_apply_post_iso_audio(h);
    }
}

void hd60s_unmute_oneshot(libusb_device_handle *h) {
    // IT6802E 0x94 bank audio unmute (hallazgo de Fable + spec de registros IT6604)
    // golpea reg 0x87 (HWMUTE_CTRL) y reg 0x89 (TRISTATE_CTRL) para untri-state de I2S
    // se activa con env HD60S_AUDIO=1
    const char* env_audio = getenv("HD60S_AUDIO");
    if (env_audio && env_audio[0] && env_audio[0] != '0' && env_audio[0] != 'n' && env_audio[0] != 'N') {
        fprintf(stderr, "[audio-unmute] IT6802E (0x94 bank) audio path unmute...\n");
        // envía una racha de comandos a IT6802E (bank select → HWMUTE_CTRL clear → TRISTATE_CTRL clear)
        struct { unsigned char slave, reg, val; const char* name; } audio_unmute[] = {
            {0x94, 0x0f, 0x8b, "reg 0x0f audio clock enable"},
            // reg 0x87 (REG_RX_HWMUTE_CTRL): bit3=HW_MUTE_EN, bit4=MUTE_CLR
            //   = 0x10 → clear (bit4 a 1, bit3 a 0)
            {0x94, 0x87, 0x10, "reg 0x87 HWMUTE clear + disable"},
            // reg 0x89 (REG_RX_TRISTATE_CTRL): untri-state de todo I2S/SPDIF
            //   = 0x00 → todo clear
            {0x94, 0x89, 0x00, "reg 0x89 TRISTATE untri all"},
        };
        for (int u = 0; u < (int)(sizeof(audio_unmute)/sizeof(audio_unmute[0])); u++) {
            unsigned char w[3] = {audio_unmute[u].slave, audio_unmute[u].reg, audio_unmute[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 200);
            fprintf(stderr, "  %s = %d\n", audio_unmute[u].name, r);
            usleep(2000);
        }
    }

    // IT6802E audio unmute V2 — registros correctos (ver DB_C10 SDK it680x_regs.h)
    // se activa con env HD60S_AUDIO_V2=1
    // Fable se basó en spec IT6604 (reg 0x87), pero en IT6802 (el chip real del HD60S)
    // REG_RX_HWMuteCtrl es 0x7D. También se golpean los otros reg relacionados.
    const char* env_audio_v2 = getenv("HD60S_AUDIO_V2");
    if (env_audio_v2 && env_audio_v2[0] && env_audio_v2[0] != '0' && env_audio_v2[0] != 'n' && env_audio_v2[0] != 'N') {
        fprintf(stderr, "[audio-unmute-v2] IT6802E (正しい reg) audio path unmute...\n");
        struct { unsigned char slave, reg, val; const char* name; } audio_unmute_v2[] = {
            {0x94, 0x0f, 0x8b, "reg 0x0f audio clock enable"},
            // REG_RX_HWMuteCtrl = 0x7D
            //   bit3=HWMuteEn (0=disable HW mute), bit4=HWMuteClr (1=clear mute)
            //   0x10 = bit4 set, bit3 clear
            {0x94, 0x7d, 0x10, "reg 0x7D REG_RX_HWMuteCtrl clear"},
            // REG_RX_074 (reg 0x74): bit2=Force_AVMute (0=clear), bit3=AVMute_Value (0)
            //   todo a 0 (clear)
            {0x94, 0x74, 0x00, "reg 0x74 Force_AVMute clear"},
            // REG_RX_0A8 (reg 0xA8): bit0=P0_AVMUTE, bit4=P1_AVMUTE → todo clear
            {0x94, 0xa8, 0x00, "reg 0xA8 AVMute (P0/P1) clear"},
            // REG_RX_07E (reg 0x7E): bit4=Force_I2SOut (=1 fuerza salida I2S ON)
            {0x94, 0x7e, 0x10, "reg 0x7E Force I2SOut ON"},
        };
        for (int u = 0; u < (int)(sizeof(audio_unmute_v2)/sizeof(audio_unmute_v2[0])); u++) {
            unsigned char w[3] = {audio_unmute_v2[u].slave, audio_unmute_v2[u].reg, audio_unmute_v2[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 200);
            fprintf(stderr, "  %s = %d\n", audio_unmute_v2[u].name, r);
            usleep(2000);
        }
    }

    // unmute mínimo: en 0x509c, bank2 select → reg 0x20 = 0x00
    // se activa con env HD60S_MIN=1
    const char* env_min = getenv("HD60S_MIN");
    if (env_min && env_min[0] && env_min[0] != '0' && env_min[0] != 'n' && env_min[0] != 'N') {
        fprintf(stderr, "[min-unmute] 0x509c 最小シーケンス (bank2 select + reg 0x20=0x00)...\n");
        // según firmware: unmute = dev=2 page reg 0x20 = 0x00
        // protocol: `00 BB` = cambio de bank (BB=bank), `RR VV` = reg RR = VV
        // se emite 6 veces (para estabilizar el estado)
        for (int cycle = 0; cycle < 6; cycle++) {
            unsigned char sel[2] = {0x00, 0x02};
            libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, sel, 2, 100);
            usleep(500);
            unsigned char wr[2] = {0x20, 0x00};
            libusb_control_transfer(h, 0x40, 0xC0, 0x509c, 0, wr, 2, 100);
            usleep(500);
        }
        fprintf(stderr, "  min-unmute done\n");
    }

    // según el firmware: writes I2C directos de audio unmute a IT6802E (bank 0x9c)
    // se activa con env HD60S_UNMUTE=1 (legacy)
    const char* env_unmute = getenv("HD60S_UNMUTE");
    if (env_unmute && env_unmute[0] && env_unmute[0] != '0' && env_unmute[0] != 'n' && env_unmute[0] != 'N') {
        fprintf(stderr, "[unmute] IT6802E audio unmute シーケンス投入...\n");
        struct { unsigned char reg, val; const char* name; } audio_unmute[] = {
            {0x1B, 0xFF, "reg 0x1B ch enable all"},
            {0x02, 0xF5, "reg 0x02 AUD_EN (bit7 set)"},
            {0x27, 0x00, "reg 0x27 output mute clear"},
            {0x07, 0x04, "reg 0x07 audio path enable"},
            {0x25, 0xA2, "reg 0x25 audio ctrl"},
        };
        for (int u = 0; u < (int)(sizeof(audio_unmute)/sizeof(audio_unmute[0])); u++) {
            unsigned char w[3] = {0x9c, audio_unmute[u].reg, audio_unmute[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 1000);
            fprintf(stderr, "  %s = %d\n", audio_unmute[u].name, r);
            usleep(2000);
        }
        // también unmute del audio ctrl del lado IT66121 (0x9a)
        struct { unsigned char reg, val; const char* name; } tx_audio[] = {
            {0xC1, 0x00, "reg 0xC1 AVMUTE clear"},
            {0xB9, 0x03, "reg 0xB9 audio InfoFrame enable"},
            {0xE0, 0x01, "reg 0xE0 audio ctrl"},
            {0xE4, 0x10, "reg 0xE4 audio config"},
        };
        for (int u = 0; u < (int)(sizeof(tx_audio)/sizeof(tx_audio[0])); u++) {
            unsigned char w[3] = {0x9a, tx_audio[u].reg, tx_audio[u].val};
            int r = libusb_control_transfer(h, 0x40, 0xC0, 0x5066, 0, w, 3, 1000);
            fprintf(stderr, "  IT66121 %s = %d\n", tx_audio[u].name, r);
            usleep(2000);
        }
    }
}
