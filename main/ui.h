#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    UI_STATE_IDLE,
    UI_STATE_BUSY,
    UI_STATE_OK,
    UI_STATE_ERROR,
} ui_state_t;

/* Bring up the LCD and draw the static layout. Safe to call once, from app_main. */
esp_err_t ui_init(void);

void ui_set_wifi(const char *ssid, const char *ip);
void ui_set_status(ui_state_t state, const char *text);

/* Show the retrieved network. Never displays key material. */
void ui_show_dataset(const char *net_name, int channel, uint16_t panid);
void ui_clear_dataset(void);

/*
 * Run the camera self-test on the UI worker task and block until it finishes.
 * The console task's stack is far too small for quirc; the worker is already
 * sized for it. False if the UI (and therefore the worker) never started.
 */
bool ui_run_camtest(void);

/* Drop the remembered network from the screen and NVS. */
void ui_forget_network(void);
