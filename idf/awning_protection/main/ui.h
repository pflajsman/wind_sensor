/* ui.h: user button (IO25) + status LED (SK6812 on IO22) for the awning sensor.
 *
 * The button is NOT a light-sleep wake source (IO25 is not an LP/RTC GPIO), so it is sampled
 * on every report-task tick (ui_tick(), once per MOTION_POLL_S). A press therefore has to be
 * held for up to ~1 s before it is seen. Once seen, the UI task keeps the CPU awake and times
 * the press precisely until release.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef enum {
    UI_LED_OFF = 0,
    UI_LED_PAIRING,       /* blue blink: pairing window open */
    UI_LED_JOINED,        /* green solid: joined / reconnected */
    UI_LED_SETUP,         /* green blink: just joined, deCONZ is setting the sensor up */
    UI_LED_ACK,           /* short green flash: short press acknowledged while joined */
    UI_LED_FAILED,        /* red solid: pairing window ran out without a network */
    UI_LED_RESET_ARMED,   /* fast red blink: long press reached, release to factory reset */
    UI_LED_RESETTING,     /* red solid: factory reset in progress (until reboot) */
    UI_LED_IDENTIFY,      /* white blink: coordinator sent Identify */
} ui_led_mode_t;

typedef struct {
    void (*on_short_press)(void);  /* released before UI_LONG_PRESS_S */
    void (*on_long_press)(void);   /* held >= UI_LONG_PRESS_S, then released */
} ui_callbacks_t;

/* Configure the button GPIO + LED driver and start the UI tasks. Non-fatal on LED failure
 * (the device still works, just without feedback). */
esp_err_t ui_init(const ui_callbacks_t *cb);

/* Show an LED pattern. duration_ms = 0 keeps it until the next call; otherwise the LED goes
 * off after duration_ms. Safe from any task context (including the Zigbee stack context);
 * never blocks. */
void ui_led(ui_led_mode_t mode, uint32_t duration_ms);

/* Sample the button. Called from the report task on every tick so the button check shares
 * the existing 1 s light-sleep wakeup instead of adding a second one. */
void ui_tick(void);
