#ifndef HD60S_V4L2_H
#define HD60S_V4L2_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int hd60s_v4l2_open(const char *devpath);
ssize_t hd60s_v4l2_write_frame(const uint8_t *frame, size_t n);
void hd60s_v4l2_close(void);
int hd60s_v4l2_is_open(void);

#endif
