/*
 * QR scanning on the CoreS3's GC0308.
 *
 * Frames come from esp_video through the V4L2 interface (the BSP's camera
 * driver), are converted to 8-bit grey for quirc, and optionally copied out as
 * RGB565 so the UI can show a live preview.
 *
 * Buffers land in PSRAM, which is why getting the quad-vs-octal setting right
 * mattered before any of this could work.
 */
#include "qrscan.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "esp_cam_sensor.h"
#include "esp_log.h"
#include "esp_video_device.h"   /* ESP_VIDEO_DVP_DEVICE_NAME, used by BSP_CAMERA_DEVICE */
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "quirc.h"

/* Private esp_video API (private_include/esp_video_device_internal.h): the
 * DVP video device owns the sensor handle; this is the only way to reach it
 * for raw register access. */
extern esp_cam_sensor_device_t *esp_video_get_dvp_video_device_sensor(void);

static const char *TAG = "qrscan";

/* Three buffers, so the sensor always has somewhere to write while we work. */
#define BUF_COUNT 3

static int s_fd = -1;
static uint8_t *s_buf[BUF_COUNT];
static size_t s_buf_len;
static uint32_t s_fourcc;
static int s_w, s_h;
static struct quirc *s_quirc;
static bool s_running;

/*
 * esp_video_init() registers "/dev/videoN" globally and is NOT idempotent --
 * a second call fails with "video name=DVP id=2 has been registered". So the
 * camera is brought up exactly once for the lifetime of the process, and
 * start/stop only opens and closes the V4L2 device.
 */
static bool s_video_inited;

/* Stats from the most recent frame, for diagnosing decode failures. */
static uint8_t s_min = 255, s_max, s_mean;
static int s_candidates;

void qrscan_last_frame_stats(uint8_t *min, uint8_t *max, uint8_t *mean, int *candidates)
{
    if (min) { *min = s_min; }
    if (max) { *max = s_max; }
    if (mean) { *mean = s_mean; }
    if (candidates) { *candidates = s_candidates; }
}

int qrscan_width(void) { return s_w; }
int qrscan_height(void) { return s_h; }
bool qrscan_active(void) { return s_running; }

static inline uint8_t rgb565_luma(uint16_t p)
{
    /* 5/6/5 -> 8 bit each, then ITU-R 601 luma. */
    uint8_t r = (uint8_t) (((p >> 11) & 0x1F) << 3);
    uint8_t g = (uint8_t) (((p >> 5) & 0x3F) << 2);
    uint8_t b = (uint8_t) ((p & 0x1F) << 3);
    return (uint8_t) ((77u * r + 151u * g + 28u * b) >> 8);
}

/* Keeps a copy of the last luma frame purely so it can be dumped for eyeballing. */
static uint8_t *s_last_grey;

void qrscan_dump_ascii(void)
{
    if (!s_last_grey || s_w == 0 || s_h == 0) {
        printf("no frame captured yet\n");
        return;
    }
    const char *ramp = "@%#*+=-:. ";   /* dark -> light */
    const int cols = 64, rows = 30;

    printf("--- %dx%d luma, rendered %dx%d ---\n", s_w, s_h, cols, rows);
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            /* Box-average the source region so thin features survive. */
            int x0 = c * s_w / cols, x1 = (c + 1) * s_w / cols;
            int y0 = r * s_h / rows, y1 = (r + 1) * s_h / rows;
            uint32_t sum = 0, n = 0;
            for (int y = y0; y < y1; y++) {
                for (int x = x0; x < x1; x++) {
                    sum += s_last_grey[(size_t) y * s_w + x];
                    n++;
                }
            }
            uint8_t v = n ? (uint8_t) (sum / n) : 0;
            putchar(ramp[(v * 9) / 255]);
        }
        putchar('\n');
    }
    printf("--- end frame ---\n");
}

/* Full teardown. Only for the cold-init failure path: doing this on a healthy
 * pipeline corrupts internal heap (frame-sized values were found written over
 * the console REPL struct, panicking in xQueueGenericSend after every
 * camtest), so a started pipeline is kept for the life of the process and
 * qrscan_stop() only halts streaming. */
static void qrscan_teardown(void)
{
    if (s_fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(s_fd, VIDIOC_STREAMOFF, &type);
        close(s_fd);
        s_fd = -1;
    }
    if (s_quirc) {
        quirc_destroy(s_quirc);
        s_quirc = NULL;
    }
    memset(s_buf, 0, sizeof(s_buf));
    s_running = false;
}

void qrscan_selftest(void)
{
    if (qrscan_start() != ESP_OK) {
        printf(">>> camera FAILED to start (see errors above) <<<\n");
        return;
    }
    printf("camera started: %dx%d\n", qrscan_width(), qrscan_height());

    char payload[256];
    int decoded = 0, frames = 0;
    for (int i = 0; i < 40; i++) {
        frames++;
        if (qrscan_poll(payload, sizeof(payload), NULL)) {
            printf("decoded: %s\n", payload);
            decoded = 1;
            break;
        }
        if ((i % 10) == 9) {
            uint8_t mn, mx, mean;
            int cand;
            qrscan_last_frame_stats(&mn, &mx, &mean, &cand);
            printf("  frame %2d: luma min=%3u max=%3u mean=%3u  qr_candidates=%d\n",
                   frames, mn, mx, mean, cand);
        }
        vTaskDelay(pdMS_TO_TICKS(10));   /* let other tasks breathe */
    }
    printf("frames=%d decoded=%d\n", frames, decoded);
    qrscan_dump_ascii();
    qrscan_stop();
    printf("selftest stack headroom: %u bytes\n",
           (unsigned) uxTaskGetStackHighWaterMark(NULL));
    printf(">>> camera OK <<<\n");
}

esp_err_t qrscan_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    /* Warm restart: the pipeline survived the last stop, so just re-queue the
     * buffers (STREAMOFF hands them all back) and stream again. */
    if (s_fd >= 0) {
        for (int i = 0; i < BUF_COUNT; i++) {
            struct v4l2_buffer buf = {
                .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                .memory = V4L2_MEMORY_MMAP,
                .index = i,
            };
            if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
                ESP_LOGE(TAG, "warm restart QBUF %d failed", i);
                return ESP_FAIL;
            }
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
            ESP_LOGE(TAG, "warm restart STREAMON failed");
            return ESP_FAIL;
        }
        s_running = true;
        return ESP_OK;
    }

    if (!s_video_inited) {
        esp_err_t err = bsp_camera_start(NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bsp_camera_start: %s", esp_err_to_name(err));
            return err;
        }
        s_video_inited = true;
    }

    s_fd = open(BSP_CAMERA_DEVICE, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "open %s failed", BSP_CAMERA_DEVICE);
        return ESP_FAIL;
    }

    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT failed");
        goto fail;
    }
    s_w = fmt.fmt.pix.width;
    s_h = fmt.fmt.pix.height;
    s_fourcc = fmt.fmt.pix.pixelformat;
    ESP_LOGI(TAG, "capture %dx%d fourcc %c%c%c%c", s_w, s_h,
             (char) (s_fourcc & 0xff), (char) ((s_fourcc >> 8) & 0xff),
             (char) ((s_fourcc >> 16) & 0xff), (char) ((s_fourcc >> 24) & 0xff));

    struct v4l2_requestbuffers req = {
        .count = BUF_COUNT,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed");
        goto fail;
    }

    for (int i = 0; i < BUF_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF %d failed", i);
            goto fail;
        }
        s_buf[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, buf.m.offset);
        if (s_buf[i] == NULL) {
            ESP_LOGE(TAG, "mmap %d failed", i);
            goto fail;
        }
        s_buf_len = buf.length;
        if (ioctl(s_fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF %d failed", i);
            goto fail;
        }
    }

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed");
        goto fail;
    }

    /*
     * The 320x240 subsample init table ships with horizontal mirror ON
     * (reg 0x14 = 0x11, vs 0x10 in the 640x480 table). QR codes are not
     * mirror-invariant and quirc never tries the flipped image, so with the
     * mirror set decoding is impossible no matter how good the frame is.
     *
     * Written via the raw-register ioctl on the sensor handle, not
     * VIDIOC_S_EXT_CTRLS: with the ext-ctrl path the board panicked in the
     * console task after every camtest (frame-sized garbage over the REPL
     * struct), and the direct write avoids that machinery entirely.
     */
    {
        esp_cam_sensor_device_t *cam = esp_video_get_dvp_video_device_sensor();
        esp_cam_sensor_reg_val_t reg = { .regaddr = 0x14, .value = 0x10 };
        if (cam == NULL ||
            esp_cam_sensor_ioctl(cam, ESP_CAM_SENSOR_IOC_S_REG, &reg) != ESP_OK) {
            ESP_LOGW(TAG, "failed to clear sensor mirror - QR decode will fail");
        }
    }

    s_quirc = quirc_new();
    if (s_quirc == NULL || quirc_resize(s_quirc, s_w, s_h) < 0) {
        ESP_LOGE(TAG, "quirc alloc failed (%dx%d)", s_w, s_h);
        goto fail;
    }

    /* PSRAM: only used for the diagnostic dump, so failure is not fatal. */
    if (s_last_grey == NULL) {
        s_last_grey = heap_caps_malloc((size_t) s_w * s_h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    s_running = true;
    ESP_LOGI(TAG, "scanner running");
    return ESP_OK;

fail:
    qrscan_teardown();
    return ESP_FAIL;
}

void qrscan_stop(void)
{
    if (!s_running) {
        return;
    }
    /* Streaming off only -- fd, mmaps and the decoder stay alive. See
     * qrscan_teardown() for why full teardown is reserved for init failure. */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(s_fd, VIDIOC_STREAMOFF, &type);
    s_running = false;
}

bool qrscan_poll(char *out, size_t cap, uint16_t *preview_rgb565)
{
    if (!s_running) {
        return false;
    }

    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
        return false;
    }

    const uint8_t *src = s_buf[buf.index];
    int qw = s_w, qh = s_h;
    uint8_t *grey = quirc_begin(s_quirc, &qw, &qh);
    const size_t px = (size_t) s_w * s_h;

    if (s_fourcc == V4L2_PIX_FMT_GREY) {
        memcpy(grey, src, px);
    } else if (s_fourcc == V4L2_PIX_FMT_YUYV) {
        /* Y is every other byte. */
        for (size_t i = 0; i < px; i++) {
            grey[i] = src[i * 2];
        }
    } else {
        /* Treat anything else as RGB565; swap bytes for the big-endian variant. */
        const bool swap = (s_fourcc == V4L2_PIX_FMT_RGB565X);
        const uint16_t *p16 = (const uint16_t *) src;
        for (size_t i = 0; i < px; i++) {
            uint16_t p = p16[i];
            if (swap) {
                p = (uint16_t) ((p >> 8) | (p << 8));
            }
            grey[i] = rgb565_luma(p);
        }
    }

    /*
     * Preview built from the luma plane, scaled to fit the canvas height and
     * letterboxed. Cropping instead (the old behaviour) hid the bottom 90 rows,
     * so a code centred in front of the lens sat half outside the preview.
     */
    if (preview_rgb565) {
        const int ph = QRSCAN_PREVIEW_H;
        const int pw = s_w * ph / s_h;                 /* 200 for a 4:3 frame */
        const int x0 = (QRSCAN_PREVIEW_W - pw) / 2;
        for (int y = 0; y < ph; y++) {
            uint16_t *dst = preview_rgb565 + (size_t) y * QRSCAN_PREVIEW_W;
            const uint8_t *srow = grey + (size_t) (y * s_h / ph) * s_w;
            for (int x = 0; x < x0; x++) { dst[x] = 0; }
            for (int x = 0; x < pw; x++) {
                uint8_t g = srow[x * s_w / pw];
                dst[x0 + x] = (uint16_t) (((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3));
            }
            for (int x = x0 + pw; x < QRSCAN_PREVIEW_W; x++) { dst[x] = 0; }
        }
    }

    /* Luma spread tells us whether the sensor is producing a usable image:
     * a near-flat min/max means dark or washed out, and no amount of decoder
     * tuning will help. */
    {
        uint32_t sum = 0;
        uint8_t lo = 255, hi = 0;
        for (size_t i = 0; i < px; i += 7) {   /* sampled; full scan is wasteful */
            uint8_t g = grey[i];
            sum += g;
            if (g < lo) { lo = g; }
            if (g > hi) { hi = g; }
        }
        s_min = lo;
        s_max = hi;
        s_mean = (uint8_t) (sum / ((px + 6) / 7));
    }

    if (s_last_grey) {
        memcpy(s_last_grey, grey, px);
    }

    /*
     * Hand the buffer back BEFORE decoding. quirc takes hundreds of
     * milliseconds on a 320x240 frame, and holding the buffer across that
     * starves the DVP of somewhere to write -- which stalls capture and makes
     * the preview freeze in bursts.
     */
    ioctl(s_fd, VIDIOC_QBUF, &buf);

    quirc_end(s_quirc);

    bool got = false;
    int n = quirc_count(s_quirc);
    s_candidates = n;
    for (int i = 0; i < n && !got; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(s_quirc, i, &code);
        if (quirc_decode(&code, &data) == QUIRC_SUCCESS) {
            size_t len = data.payload_len < cap - 1 ? data.payload_len : cap - 1;
            memcpy(out, data.payload, len);
            out[len] = '\0';
            got = true;
            ESP_LOGI(TAG, "decoded %u bytes", (unsigned) len);
        }
    }

    return got;
}
