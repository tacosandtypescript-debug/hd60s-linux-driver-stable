#include "hd60s_v4l2.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#define FRAME_W 1920
#define FRAME_H 1080
#define LINE_BYTES (FRAME_W * 2)
#define FRAME_BYTES (LINE_BYTES * FRAME_H)

static int g_v4l_fd = -1;

int hd60s_v4l2_is_open(void) {
    return g_v4l_fd >= 0;
}

int hd60s_v4l2_open(const char* devpath) {
    // Nonblocking prevents the userspace capture loop from deadlocking when
    // OBS has not opened the consumer side yet.
    int fd = open(devpath, O_WRONLY | O_NONBLOCK);
    if (fd < 0) { perror("open v4l2loopback"); g_v4l_fd = -1; return -1; }
    // 🔥 2026-07-11 diagnóstico Opus 4.8: con solo write() la máquina de estado de timestamp
    // de v4l2loopback no se inicializa y el consumer deja de mostrar frames (síntomas: VLC "Timestamp conversion
    // failed", ffplay fd=0, hang de mpv, etc.). Hay que hacer VIDIOC_S_FMT + STREAMON explícitos para
    // "arrancar oficialmente" el stream OUTPUT.
    struct v4l2_format vf; memset(&vf, 0, sizeof(vf));
    vf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    vf.fmt.pix.width = 1920;
    vf.fmt.pix.height = 1080;
    vf.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    vf.fmt.pix.field = V4L2_FIELD_NONE;
    vf.fmt.pix.bytesperline = 3840;
    vf.fmt.pix.sizeimage = 4147200;
    vf.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
    if (ioctl(fd, VIDIOC_S_FMT, &vf) < 0) {
        fprintf(stderr, "[v4l2] S_FMT 失敗: %s\n", strerror(errno));
        close(fd);
        g_v4l_fd = -1;
        return -1;
    }

    // S_FMT is a negotiation: the driver is allowed to change every field.
    // Verify the negotiated format before any frame is written.  Accepting a
    // different stride or size here would make a valid YUYV frame appear as a
    // vertical roll or horizontal bands in the consumer.
    struct v4l2_format actual;
    memset(&actual, 0, sizeof(actual));
    actual.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(fd, VIDIOC_G_FMT, &actual) < 0) {
        fprintf(stderr, "[v4l2] G_FMT 失敗: %s\n", strerror(errno));
        close(fd);
        g_v4l_fd = -1;
        return -1;
    }
    fprintf(stderr,
            "[v4l2] negotiated %ux%u pixfmt=0x%08x field=%u bytesperline=%u sizeimage=%u\n",
            actual.fmt.pix.width, actual.fmt.pix.height,
            actual.fmt.pix.pixelformat, actual.fmt.pix.field,
            actual.fmt.pix.bytesperline, actual.fmt.pix.sizeimage);
    if (actual.fmt.pix.width != FRAME_W || actual.fmt.pix.height != FRAME_H ||
        actual.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
        actual.fmt.pix.bytesperline != LINE_BYTES ||
        actual.fmt.pix.sizeimage != FRAME_BYTES) {
        fprintf(stderr,
                "[v4l2] incompatible negotiated format; refusing frame writes\n");
        close(fd);
        g_v4l_fd = -1;
        return -1;
    }

    struct v4l2_streamparm sp; memset(&sp, 0, sizeof(sp));
    sp.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    sp.parm.output.timeperframe.numerator = 1;
    sp.parm.output.timeperframe.denominator = 60;
    if (ioctl(fd, VIDIOC_S_PARM, &sp) < 0) fprintf(stderr, "[v4l2] S_PARM fps60 失敗(続行)\n");

    // v4l2loopback's write() interface owns the OUTPUT queue.  Calling
    // VIDIOC_STREAMON here can block waiting for a queue state that is
    // established by the consumer (OBS), so leave the queue in write mode.
    // A complete frame is submitted by each write(FRAME_BYTES) below.
    fprintf(stderr, "[v4l2] %s opened (YUYV 1920x1080 @60fps, S_FMT/write mode)\n", devpath);
    g_v4l_fd = fd;
    return fd;
}

ssize_t hd60s_v4l2_write_frame(const uint8_t *frame, size_t n) {
    if (g_v4l_fd < 0) return -1;
    return write(g_v4l_fd, frame, n);
}

void hd60s_v4l2_close(void) {
    if (g_v4l_fd < 0) return;
    enum v4l2_buf_type btype = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(g_v4l_fd, VIDIOC_STREAMOFF, &btype);
    close(g_v4l_fd);
    g_v4l_fd = -1;
}
