/* Exercise the production poll and diagnostic using deterministic driver events. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define QRSCAN_PREVIEW_H 150
#define QRSCAN_PREVIEW_W 320
enum { V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, V4L2_PIX_FMT_GREY,
       V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_RGB565X, VIDIOC_DQBUF, VIDIOC_QBUF };
#define V4L2_BUF_FLAG_ERROR 1
struct v4l2_buffer { unsigned type, memory, index, flags, bytesused; };
struct quirc { int unused; };
struct quirc_code { int unused; };
struct quirc_data { size_t payload_len; char payload[2]; };
typedef int quirc_decode_error_t;
enum { QUIRC_SUCCESS, QUIRC_ERROR_DATA_ECC };

static const char *scenario;
static int dequeues, requeues, decodes, stops, dumps;
static int64_t now;
static uint8_t pixels[16], grey[16];
static uint8_t *s_last_grey, s_min, s_max, s_mean;
static int s_candidates;
static bool is(const char *name) { return strcmp(scenario, name) == 0; }

static int ioctl(int fd, int request, struct v4l2_buffer *buf)
{
    assert(fd == 1);
    if (request == VIDIOC_DQBUF) {
        ++dequeues;
        if (is("dequeue") && dequeues == 4) return -1;
        buf->index = is("bad-index") ? 8 : 0;
        buf->bytesused = is("short") ? sizeof(pixels) - 1 : sizeof(pixels);
        buf->flags = is("flag") ? V4L2_BUF_FLAG_ERROR : 0;
        return 0;
    }
    assert(request == VIDIOC_QBUF && buf->index == 0);
    ++requeues;
    return is("requeue") ? -1 : 0;
}

static uint8_t *quirc_begin(struct quirc *q, int *w, int *h)
{ (void)q; assert(*w == 4 && *h == 4); return grey; }
static void quirc_end(struct quirc *q) { (void)q; ++decodes; }
static int quirc_count(struct quirc *q) { (void)q; return is("qr") || is("slow"); }
static void quirc_extract(struct quirc *q, int i, struct quirc_code *c)
{ (void)q; (void)c; assert(i == 0); }
static quirc_decode_error_t quirc_decode(struct quirc_code *c, struct quirc_data *d)
{
    (void)c;
    if (is("slow")) now += 60000000;
    d->payload_len = 1;
    d->payload[0] = 'x';
    return QUIRC_SUCCESS;
}
static void quirc_flip(struct quirc_code *c) { (void)c; }
static uint8_t rgb565_luma(uint16_t p) { (void)p; assert(0); return 0; }
static int64_t esp_timer_get_time(void) { return now; }
#define pdMS_TO_TICKS(ms) (ms)
static void vTaskDelay(int ms) { now += ms * 1000; }
static int uxTaskGetStackHighWaterMark(void *task) { (void)task; return 100; }
static int qrscan_start(void);
static int qrscan_width(void) { return 4; }
static int qrscan_height(void) { return 4; }
static void qrscan_stop(void) { ++stops; }
static void qrscan_dump_ascii(void) { ++dumps; }
static void qrscan_last_frame_stats(uint8_t *min, uint8_t *max, uint8_t *mean, int *candidates)
{ *min = s_min; *max = s_max; *mean = s_mean; *candidates = s_candidates; }

#include "camera_under_test.inc"

#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_SPIRAM 3
static size_t heap_caps_get_free_size(int cap) { (void)cap; return 0; }
static size_t heap_caps_get_largest_free_block(int cap) { (void)cap; return 0; }
static bool qrscan_active(void) { return false; }
static bool ui_run_on_worker(void (*fn)(void *), void *arg, uint32_t timeout_ms)
{
    assert(timeout_ms == 120000);
    fn(arg);
    return true;
}

#include "camera_console_under_test.inc"

static int qrscan_start(void)
{
    s_running = true;
    s_fd = 1;
    s_w = s_h = 4;
    s_fourcc = V4L2_PIX_FMT_GREY;
    s_buf[0] = pixels;
    s_buf_len = sizeof(pixels);
    return ESP_OK;
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    scenario = argv[1];
    assert(cmd_camtest(0, NULL) == ((is("good") || is("qr")) ? 0 : 1));
    assert(stops == 1 && dumps == (decodes > 0));
    assert(requeues == dequeues - (is("dequeue") || is("bad-index")));
    assert(decodes == requeues - (is("short") || is("flag") || is("requeue")));
    return 0;
}
