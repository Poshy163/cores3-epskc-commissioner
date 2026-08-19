#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Start the camera and the QR decoder. Idempotent. */
esp_err_t qrscan_start(void);
void qrscan_stop(void);

/* True while a scan owns the camera. Callers other than the owner must not
 * poll or stop it: tearing the decoder down under an in-flight poll is a
 * use-after-free (observed as a LoadProhibited panic). */
bool qrscan_active(void);

/* Capture geometry, valid after qrscan_start(). */
int qrscan_width(void);
int qrscan_height(void);

/*
 * Preview geometry: the UI gives the camera 150 rows of the 240-row screen,
 * so the full 4:3 frame is scaled down and letterboxed into this area rather
 * than cropped.
 */
#define QRSCAN_PREVIEW_W 320
#define QRSCAN_PREVIEW_H 150

/*
 * Grab one frame and try to decode a QR code from it.
 *   returns true  -> `out` holds the decoded payload
 *   returns false -> nothing decoded this frame (normal; just call again)
 *
 * If `preview_rgb565` is non-NULL it receives the whole frame scaled into
 * QRSCAN_PREVIEW_W x QRSCAN_PREVIEW_H (letterboxed), and must be at least
 * QRSCAN_PREVIEW_W * QRSCAN_PREVIEW_H * 2 bytes.
 */
bool qrscan_poll(char *out, size_t cap, uint16_t *preview_rgb565);

/*
 * Luma statistics for the most recent frame, plus how many QR candidates quirc
 * located. Distinguishes "no image at all" from "image fine, decode failing".
 */
void qrscan_last_frame_stats(uint8_t *min, uint8_t *max, uint8_t *mean, int *candidates);

/*
 * Print the most recent frame as ASCII art over the console. Seeing the actual
 * image settles focus/exposure/orientation questions that statistics cannot.
 */
void qrscan_dump_ascii(void);

/*
 * Capture 40 frames, report luma statistics and dump the last frame.
 *
 * quirc needs far more stack than the console task has, so this must run on a
 * task sized for it -- the UI worker. Calling it from the console task
 * overflows the stack and corrupts memory that only faults much later.
 */
void qrscan_selftest(void);
