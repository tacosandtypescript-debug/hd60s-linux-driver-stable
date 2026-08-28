#ifndef HD60S_AUDIO_H
#define HD60S_AUDIO_H

#include <stdio.h>
#include <stdint.h>

void hd60s_audio_open(const char *alsa_dev);
void hd60s_audio_feed_sep(const uint8_t *payload12);
void hd60s_audio_close(void);
void hd60s_audio_dump_stats(FILE *fp);

#endif
