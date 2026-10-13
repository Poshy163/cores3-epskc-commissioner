#include "log_ring.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
/* Without it EXT_RAM_NOINIT_ATTR silently places the ring in internal RAM,
 * which this board cannot spare. */
#error "log_ring needs CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY"
#endif

#define RING_MAGIC 0x52474f4cu   /* "LOGR" */
#define HALF_MAGIC 0x48474f4cu   /* "LOGH" */
#define PENDING_SLOTS 4
#define FORMAT_BUFFER 192
#define MSYNC_FLAGS (ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | \
                     ESP_CACHE_MSYNC_FLAG_UNALIGNED)

typedef struct {
    uint32_t seq;          /* zero while the slot is being rewritten */
    uint32_t uptime_ms;
    uint32_t check;        /* covers the boot ID, so stale slots never validate */
    char text[LOG_RING_TEXT];
} slot_t;
_Static_assert(sizeof(slot_t) == 192, "a slot must stay whole cache lines");

typedef struct {
    uint32_t magic;
    uint32_t boot_count;
    uint64_t boot_id;
    uint8_t reserved[48];
    slot_t slots[LOG_RING_LINES];
} half_t;

typedef struct {
    uint32_t magic;
    uint32_t layout;       /* a build with a different layout is not trusted */
    uint32_t boot_count;
    uint32_t active;       /* half written by the newest boot */
    uint8_t reserved[48];
    half_t half[2];
} ring_t;
_Static_assert(offsetof(ring_t, half) % 64 == 0 && offsetof(half_t, slots) % 64 == 0,
               "slots must start on a cache line");

/* Survives software, panic and watchdog resets; garbage after power-on. */
static EXT_RAM_NOINIT_ATTR __attribute__((aligned(64))) ring_t s_ring;

/* A line some callers (OpenThread) emit as several esp_log calls, held until
 * its newline arrives. One per task that is mid-line. */
typedef struct {
    TaskHandle_t task;
    size_t len;
    char text[LOG_RING_TEXT];
} pending_t;
static EXT_RAM_BSS_ATTR pending_t s_pending[PENDING_SLOTS];
static unsigned s_pending_victim;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static half_t *s_cur, *s_prev;
static uint32_t s_latest, s_last_ms;
static uint32_t s_prev_latest, s_prev_last_ms;
static bool s_writeback;
static vprintf_like_t s_console;

static uint32_t slot_check(const slot_t *s, uint64_t boot_id)
{
    uint32_t h = 2166136261u;
    const uint32_t words[4] = { s->seq, s->uptime_ms, (uint32_t) boot_id, (uint32_t) (boot_id >> 32) };
    const uint8_t *bytes = (const uint8_t *) words;
    for (size_t i = 0; i < sizeof(words); ++i) {
        h = (h ^ bytes[i]) * 16777619u;
    }
    for (size_t i = 0; i < LOG_RING_TEXT && s->text[i]; ++i) {
        h = (h ^ (uint8_t) s->text[i]) * 16777619u;
    }
    return h;
}

static bool slot_valid(const slot_t *s, uint64_t boot_id)
{
    return s->seq != 0 && memchr(s->text, '\0', LOG_RING_TEXT) != NULL &&
           s->check == slot_check(s, boot_id);
}

/* Length of the UTF-8 character starting at s[0], or zero if it is invalid or
 * runs past len. Rejects overlongs and surrogates, as strict decoders do. */
static size_t utf8_len(const unsigned char *s, size_t len)
{
    size_t n = s[0] >= 0xc2 && s[0] <= 0xdf ? 2 : s[0] >= 0xe0 && s[0] <= 0xef ? 3 :
               s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
    if (n == 0 || n > len) {
        return 0;
    }
    for (size_t i = 1; i < n; ++i) {
        if ((s[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] > 0x9f) ||
        (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] > 0x8f)) {
        return 0;
    }
    return n;
}

/* Append src to dst[used], dropping CR and ANSI colour codes, blanking other
 * control bytes and replacing invalid UTF-8. Never splits a character. */
static size_t append_clean(char *dst, size_t used, const char *src, size_t len)
{
    const size_t cap = LOG_RING_TEXT - 1;
    size_t i = 0;
    while (i < len && used < cap) {
        const unsigned char c = (unsigned char) src[i];
        if (c == 0x1b) {
            ++i;
            if (i < len && src[i] == '[') {
                for (++i; i < len && ((unsigned char) src[i] < 0x40 || (unsigned char) src[i] > 0x7e); ++i) {
                }
                ++i;
            }
        } else if (c == '\r') {
            ++i;
        } else if (c < 0x20 || c == 0x7f) {
            dst[used++] = ' ';
            ++i;
        } else if (c < 0x80) {
            dst[used++] = (char) c;
            ++i;
        } else {
            size_t n = utf8_len((const unsigned char *) src + i, len - i);
            if (n == 0) {
                dst[used++] = '?';
                ++i;
            } else if (used + n > cap) {
                break;
            } else {
                memcpy(dst + used, src + i, n);
                used += n;
                i += n;
            }
        }
    }
    dst[used] = '\0';
    return used;
}

/* Caller holds s_lock. The slot is invalid until finish_locked(). */
static slot_t *begin_locked(uint32_t *seq)
{
    *seq = ++s_latest;
    slot_t *s = &s_cur->slots[(*seq - 1) % LOG_RING_LINES];
    s->seq = 0;
    s->text[0] = '\0';
    return s;
}

static void finish_locked(slot_t *s, uint32_t seq, uint32_t now_ms)
{
    s->uptime_ms = now_ms;
    s->seq = seq;
    s->check = slot_check(s, s_cur->boot_id);
    s_last_ms = now_ms;
}

/* The ESP32-S3 reset paths leave dirty PSRAM lines in the data cache; push
 * committed slots out so a crash right after a line still keeps it. */
static void write_back(uint32_t first, uint32_t last)
{
    if (!s_writeback || first == 0) {
        return;
    }
    if (last - first >= LOG_RING_LINES) {
        esp_cache_msync(s_cur->slots, sizeof(s_cur->slots), MSYNC_FLAGS);
        return;
    }
    for (uint32_t seq = first;; ++seq) {
        esp_cache_msync(&s_cur->slots[(seq - 1) % LOG_RING_LINES], sizeof(slot_t), MSYNC_FLAGS);
        if (seq == last) {
            break;
        }
    }
}

static void note_written(uint32_t seq, uint32_t *first, uint32_t *last)
{
    if (*first == 0) {
        *first = seq;
    }
    *last = seq;
}

/* Caller holds s_lock. Commits pending text (if any) followed by seg. */
static void commit_locked(pending_t *p, const char *seg, size_t seg_len, uint32_t now_ms,
                          uint32_t *first, uint32_t *last)
{
    uint32_t seq;
    slot_t *s = begin_locked(&seq);
    size_t used = 0;
    if (p) {
        memcpy(s->text, p->text, p->len + 1);
        used = p->len;
        p->task = NULL;
        p->len = 0;
    }
    append_clean(s->text, used, seg, seg_len);
    finish_locked(s, seq, now_ms);
    note_written(seq, first, last);
}

static pending_t *find_pending_locked(TaskHandle_t task)
{
    for (size_t i = 0; i < PENDING_SLOTS; ++i) {
        if (s_pending[i].task == task) {
            return &s_pending[i];
        }
    }
    return NULL;
}

static pending_t *claim_pending_locked(TaskHandle_t task, uint32_t now_ms,
                                       uint32_t *first, uint32_t *last)
{
    for (size_t i = 0; i < PENDING_SLOTS; ++i) {
        if (s_pending[i].task == NULL) {
            s_pending[i].task = task;
            s_pending[i].len = 0;
            s_pending[i].text[0] = '\0';
            return &s_pending[i];
        }
    }
    /* Every slot is mid-line: keep the oldest as a line of its own. */
    pending_t *victim = &s_pending[s_pending_victim++ % PENDING_SLOTS];
    commit_locked(victim, "", 0, now_ms, first, last);
    victim->task = task;
    return victim;
}

/* Console text as esp_log produced it: any mix of whole and partial lines. */
static void ingest(const char *text, size_t len)
{
    if (s_cur == NULL || len == 0) {
        return;
    }
    const TaskHandle_t task = xTaskGetCurrentTaskHandle();
    const uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
    uint32_t first = 0, last = 0;

    portENTER_CRITICAL(&s_lock);
    pending_t *p = find_pending_locked(task);
    size_t start = 0;
    for (size_t i = 0; i <= len; ++i) {
        if (i < len && text[i] != '\n') {
            continue;
        }
        const char *seg = text + start;
        const size_t seg_len = i - start;
        if (i < len) {
            if (seg_len || (p && p->len)) {
                commit_locked(p, seg, seg_len, now_ms, &first, &last);
            } else if (p) {
                p->task = NULL;
            }
            p = NULL;
        } else if (seg_len) {
            if (p == NULL) {
                p = claim_pending_locked(task, now_ms, &first, &last);
            }
            p->len = append_clean(p->text, p->len, seg, seg_len);
        }
        start = i + 1;
    }
    portEXIT_CRITICAL(&s_lock);
    write_back(first, last);
}

/* esp_log's output hook. Formats once, then hands the same bytes to the
 * console, so the extra stack is one small buffer rather than a second
 * vprintf. Lines too long for the buffer still reach the console whole. */
static int capture_vprintf(const char *fmt, va_list args)
{
    if (s_console == NULL) {
        /* Another task logged while init was still installing the hook. */
        return vprintf(fmt, args);
    }
    char line[FORMAT_BUFFER];
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n < 0) {
        return s_console(fmt, args);
    }
    if ((size_t) n < sizeof(line)) {
        ingest(line, (size_t) n);
        if (s_console == vprintf) {
            return (int) fwrite(line, 1, (size_t) n, stdout);
        }
    } else {
        ingest(line, sizeof(line) - 1);
        const size_t fmt_len = strlen(fmt);
        if (fmt_len && fmt[fmt_len - 1] == '\n') {
            ingest("\n", 1);
        }
    }
    return s_console(fmt, args);
}

static void recover_previous(half_t *h)
{
    uint32_t latest = 0, last_ms = 0;
    for (size_t i = 0; i < LOG_RING_LINES; ++i) {
        const slot_t *s = &h->slots[i];
        if (s->seq > latest && slot_valid(s, h->boot_id)) {
            latest = s->seq;
            last_ms = s->uptime_ms;
        }
    }
    s_prev = h;
    s_prev_latest = latest;
    s_prev_last_ms = last_ms;
}

esp_err_t log_ring_init(void)
{
    if (s_cur) {
        return ESP_OK;
    }
    ring_t *r = &s_ring;
    if (r->magic == RING_MAGIC && r->layout == sizeof(ring_t) && r->active < 2 &&
        r->half[r->active].magic == HALF_MAGIC &&
        r->half[r->active].boot_count == r->boot_count) {
        recover_previous(&r->half[r->active]);
        r->boot_count++;
        r->active ^= 1;
    } else {
        r->magic = RING_MAGIC;
        r->layout = sizeof(ring_t);
        r->boot_count = 1;
        r->active = 0;
    }
    half_t *h = &r->half[r->active];
    h->magic = 0;
    h->boot_count = r->boot_count;
    esp_fill_random(&h->boot_id, sizeof(h->boot_id));
    h->magic = HALF_MAGIC;

    s_writeback = esp_ptr_external_ram(r);
    if (s_writeback) {
        esp_cache_msync(r, offsetof(ring_t, half), MSYNC_FLAGS);
        esp_cache_msync(h, offsetof(half_t, slots), MSYNC_FLAGS);
    }
    s_latest = 0;
    s_cur = h;
    s_console = esp_log_set_vprintf(capture_vprintf);
    return ESP_OK;
}

void log_ring_line(const char *text, size_t len)
{
    if (s_cur == NULL || text == NULL) {
        return;
    }
    const uint32_t now_ms = (uint32_t) (esp_timer_get_time() / 1000);
    uint32_t first = 0, last = 0;
    portENTER_CRITICAL(&s_lock);
    commit_locked(NULL, text, len, now_ms, &first, &last);
    portEXIT_CRITICAL(&s_lock);
    write_back(first, last);
}

static half_t *half_for(log_ring_boot_t boot)
{
    return boot == LOG_RING_PREVIOUS ? s_prev : s_cur;
}

bool log_ring_info(log_ring_boot_t boot, log_ring_info_t *out)
{
    half_t *h = half_for(boot);
    if (h == NULL || out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    const uint32_t latest = h == s_cur ? s_latest : s_prev_latest;
    out->boot_id = h->boot_id;
    out->boot_count = h->boot_count;
    out->latest_seq = latest;
    out->oldest_seq = latest == 0 ? 0 : latest > LOG_RING_LINES ? latest - LOG_RING_LINES + 1 : 1;
    out->last_uptime_ms = h == s_cur ? s_last_ms : s_prev_last_ms;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

bool log_ring_get(log_ring_boot_t boot, uint32_t seq, log_ring_line_t *out)
{
    half_t *h = half_for(boot);
    if (h == NULL || out == NULL || seq == 0) {
        return false;
    }
    slot_t copy = { 0 };
    portENTER_CRITICAL(&s_lock);
    const uint32_t latest = h == s_cur ? s_latest : s_prev_latest;
    const bool in_range = seq <= latest && latest - seq < LOG_RING_LINES;
    if (in_range) {
        copy = h->slots[(seq - 1) % LOG_RING_LINES];
    }
    portEXIT_CRITICAL(&s_lock);
    /* A slot overwritten while a slow client paged, or torn by a crash. */
    if (!in_range || copy.seq != seq || !slot_valid(&copy, h->boot_id)) {
        return false;
    }
    out->seq = seq;
    out->uptime_ms = copy.uptime_ms;
    memcpy(out->text, copy.text, LOG_RING_TEXT);
    return true;
}

size_t log_ring_json(const log_ring_line_t *line, char *out, size_t cap)
{
    if (line == NULL || out == NULL || cap < 64) {
        return 0;
    }
    int n = snprintf(out, cap, "{\"seq\":%" PRIu32 ",\"uptime_ms\":%" PRIu32 ",\"text\":\"",
                     line->seq, line->uptime_ms);
    if (n < 0 || (size_t) n >= cap) {
        return 0;
    }
    size_t used = (size_t) n;
    const size_t limit = cap - 3;   /* closing quote, brace and NUL */
    const unsigned char *t = (const unsigned char *) line->text;
    const char *nul = memchr(line->text, '\0', LOG_RING_TEXT);
    const size_t text_len = nul ? (size_t) (nul - line->text) : LOG_RING_TEXT;
    for (size_t i = 0; i < text_len;) {
        char esc[7];
        size_t k = 1, consumed = 1;
        if (t[i] == '"' || t[i] == '\\') {
            esc[0] = '\\';
            esc[1] = (char) t[i];
            k = 2;
        } else if (t[i] < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", t[i]);
            k = 6;
        } else if (t[i] < 0x80) {
            esc[0] = (char) t[i];
        } else {
            consumed = utf8_len(t + i, text_len - i);
            if (consumed == 0) {
                esc[0] = '?';
                consumed = 1;
            } else {
                k = consumed;
                memcpy(esc, t + i, k);
            }
        }
        if (used + k > limit) {
            break;
        }
        memcpy(out + used, esc, k);
        used += k;
        i += consumed;
    }
    out[used++] = '"';
    out[used++] = '}';
    out[used] = '\0';
    return used;
}
