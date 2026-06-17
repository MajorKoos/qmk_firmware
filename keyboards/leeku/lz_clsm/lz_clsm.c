// Copyright 2021 MajorKoos (@MajorKoos)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "quantum.h"
#include "i2c_master.h"
#include <avr/io.h>
#include <string.h>
#ifdef RGBLIGHT_ENABLE
#    include "rgblight_drivers.h"
#endif

/* ------------------------------------------------------------------------- *
 *  LeeKu "L3" companion-chip lighting driver
 *
 *  This board does not drive its LEDs directly. A companion MCU handles the
 *  RGB underglow, the single-level backlight and the three lock LEDs; QMK
 *  talks to it over i2c using the "tinycmd" packet protocol below.
 *
 *  V-USB needs usbPoll() to be serviced frequently. Lighting callbacks only
 *  update shadow state and set dirty flags; housekeeping_task_kb() then drains
 *  at most one bounded i2c packet per main-loop iteration.
 * ------------------------------------------------------------------------- */

// --- tinycmd command codes ------------------------------------------------
#define TINY_CMD_CONFIG_F 0
#define TINY_CMD_THREE_LOCK_F 3
#define TINY_CMD_RGB_ALL_F 20
#define TINY_CMD_RGB_BUFFER_F 23
#define TINY_CMD_RGB_SET_PRESET_F 25
#define TINY_CMD_RGB_EFFECT_SPEED_F 26
#define TINY_CMD_LED_LEVEL_F 40
#define TINY_CMD_LED_CONFIG_PRESET_F 43

#define LED_EFFECT_ALWAYS 5
#define RGB_EFFECT_BASIC 5

#define KEY_LED_CHANNEL_ALL 0xFF
#define LEDMODE_INDEX_MAX 3
#define LED_BLOCK_MAX 5
#define LEDMODE_ARRAY_SIZE (LEDMODE_INDEX_MAX * LED_BLOCK_MAX)

// --- tinycmd packet layouts (wire format: keep PACKED) --------------------
typedef struct __attribute__((packed)) {
    uint8_t cmd_code;
    uint8_t pkt_len;
    uint8_t data[LEDMODE_ARRAY_SIZE];
} tinycmd_led_config_preset_req_t;

typedef struct __attribute__((packed)) {
    uint8_t cmd_code;
    uint8_t pkt_len;
    uint8_t channel;
    uint8_t level;
} tinycmd_led_level_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  cmd_code;
    uint8_t  pkt_len;
    uint16_t speed; // 2: fast, 3: normal, 4: slow
} tinycmd_rgb_effect_speed_req_t;

typedef struct __attribute__((packed)) {
    uint8_t index;
    uint8_t high_hold;
    uint8_t low_hold;
    uint8_t accel_mode;
} rgb_effect_param_t;

typedef struct __attribute__((packed)) {
    uint8_t            cmd_code;
    uint8_t            pkt_len;
    uint8_t            index;
    rgb_effect_param_t effect_param;
} tinycmd_rgb_set_preset_req_t;

typedef struct __attribute__((packed)) {
    uint8_t cmd_code;
    uint8_t pkt_len;
    uint8_t num;
    uint8_t offset;
    uint8_t data[RGBLIGHT_LED_COUNT * 3];
} tinycmd_rgb_buffer_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  cmd_code;
    uint8_t  pkt_len;
    uint8_t  rgb_num;
    uint16_t rgb_limit;
} tinycmd_config_req_t;

typedef struct __attribute__((packed)) {
    uint8_t cmd_code;
    uint8_t pkt_len;
    uint8_t on; // 0: off, !=0: on
    uint8_t g;
    uint8_t r;
    uint8_t b;
} tinycmd_rgb_all_req_t;

typedef struct __attribute__((packed)) {
    uint8_t cmd_code;
    uint8_t pkt_len;
    uint8_t lock;
} tinycmd_three_lock_req_t;

// Catch any accidental compiler padding of the wire structs at build time.
_Static_assert(sizeof(tinycmd_rgb_buffer_req_t) == 4 + RGBLIGHT_LED_COUNT * 3, "tinycmd rgb buffer packed size mismatch");
_Static_assert(sizeof(tinycmd_three_lock_req_t) == 3, "tinycmd three-lock packed size mismatch");
_Static_assert(sizeof(tinycmd_led_level_req_t) == 4, "tinycmd led-level packed size mismatch");

// --- shadow state ---------------------------------------------------------
static uint8_t l3_rgb[RGBLIGHT_LED_COUNT * 3]; // wire order: g,r,b per LED (see l3_rgb_set)
static uint8_t l3_backlight_level = 0;
static uint8_t l3_lock            = 0;
static bool    l3_rgb_dirty       = false;
static bool    l3_backlight_dirty = false;
static bool    l3_lock_dirty      = false;
static bool    l3_ready           = false;

// Light back-off so an absent/wedged companion is retried at a bounded rate
// instead of being hammered every loop.
static bool     l3_failed    = false;
static uint16_t l3_failed_at = 0;
#define L3_RETRY_BACKOFF_MS 100

static bool l3_send(const void *pkt, uint8_t len) {
    if (i2c_transmit(L3_I2C_ADDRESS, (const uint8_t *)pkt, len, L3_I2C_TIMEOUT) == I2C_STATUS_SUCCESS) {
        l3_failed = false;
        return true;
    }
    l3_failed    = true;
    l3_failed_at = timer_read();
    return false;
}

// --- one-time companion configuration (runs before the main loop) ---------
static void l3_configure(void) {
    // Put every backlight block into "always on" so the host-driven buffer is shown.
    tinycmd_led_config_preset_req_t preset = {
        .cmd_code = TINY_CMD_LED_CONFIG_PRESET_F,
        .pkt_len  = sizeof(preset),
    };
    memset(preset.data, LED_EFFECT_ALWAYS, sizeof(preset.data));
    l3_send(&preset, preset.pkt_len);
    wait_ms(2);

    // Number of LEDs and the global brightness clamp.
    tinycmd_config_req_t cfg = {
        .cmd_code  = TINY_CMD_CONFIG_F,
        .pkt_len   = sizeof(cfg),
        .rgb_num   = RGBLIGHT_LED_COUNT,
        .rgb_limit = RGBLIGHT_LIMIT_VAL,
    };
    l3_send(&cfg, cfg.pkt_len);
    wait_ms(2);

    // Start with the RGB off; rgblight will push the real colours via the drainer.
    tinycmd_rgb_all_req_t off = {
        .cmd_code = TINY_CMD_RGB_ALL_F,
        .pkt_len  = sizeof(off),
        .on       = 0,
        .g        = 0,
        .r        = 0,
        .b        = 0,
    };
    l3_send(&off, off.pkt_len);
    wait_ms(2);

    tinycmd_rgb_effect_speed_req_t spd = {
        .cmd_code = TINY_CMD_RGB_EFFECT_SPEED_F,
        .pkt_len  = sizeof(spd),
        .speed    = 3,
    };
    l3_send(&spd, spd.pkt_len);
    wait_ms(2);

    // Select the "basic" preset: the companion just displays the buffer we send.
    tinycmd_rgb_set_preset_req_t pre = {
        .cmd_code     = TINY_CMD_RGB_SET_PRESET_F,
        .pkt_len      = sizeof(pre),
        .index        = RGB_EFFECT_BASIC,
        .effect_param = {5, 1, 5, 1},
    };
    l3_send(&pre, pre.pkt_len);
    wait_ms(2);
}

// --- the drainer: at most one bounded i2c packet per main-loop iteration ---
static void l3_task(void) {
    if (!l3_ready) {
        return;
    }
    if (l3_failed && timer_elapsed(l3_failed_at) < L3_RETRY_BACKOFF_MS) {
        return;
    }

    // Priority: lock LEDs (user-visible state) > backlight > RGB buffer.
    if (l3_lock_dirty) {
        tinycmd_three_lock_req_t p = {.cmd_code = TINY_CMD_THREE_LOCK_F, .pkt_len = sizeof(p), .lock = l3_lock};
        if (l3_send(&p, p.pkt_len)) {
            l3_lock_dirty = false;
        }
        return;
    }
    if (l3_backlight_dirty) {
        tinycmd_led_level_req_t p = {.cmd_code = TINY_CMD_LED_LEVEL_F, .pkt_len = sizeof(p), .channel = KEY_LED_CHANNEL_ALL, .level = l3_backlight_level};
        if (l3_send(&p, p.pkt_len)) {
            l3_backlight_dirty = false;
        }
        return;
    }
    // The RGB buffer is the largest packet (4 + 14*3 = 46 bytes). Keep it as a
    // single i2c transaction so the companion's packet parser stays in sync; the
    // timeout and retry backoff bound failures on a wedged bus.
    if (l3_rgb_dirty) {
        tinycmd_rgb_buffer_req_t p = {.cmd_code = TINY_CMD_RGB_BUFFER_F, .pkt_len = sizeof(p), .num = RGBLIGHT_LED_COUNT, .offset = 0};
        memcpy(p.data, l3_rgb, sizeof(p.data));
        if (l3_send(&p, p.pkt_len)) {
            l3_rgb_dirty = false;
        }
        return;
    }
}

// --- rgblight custom driver ----------------------------------------------
#ifdef RGBLIGHT_ENABLE
static void l3_rgb_init(void) { /* i2c + companion config handled in keyboard hooks */ }

static void l3_rgb_set(int index, uint8_t r, uint8_t g, uint8_t b) {
    if (index < 0 || index >= RGBLIGHT_LED_COUNT) {
        return;
    }
    // The L3 companion lives at the WS2812 i2c address and expects WS2812-native
    // GRB byte order, which is exactly what the original firmware sent (it shipped
    // the raw rgblight/WS2812 buffer). Preserve that so existing hardware is
    // unchanged. This is the one byte-order knob to flip if a board wants RGB.
    l3_rgb[index * 3 + 0] = g;
    l3_rgb[index * 3 + 1] = r;
    l3_rgb[index * 3 + 2] = b;
}

static void l3_rgb_set_all(uint8_t r, uint8_t g, uint8_t b) {
    for (uint8_t i = 0; i < RGBLIGHT_LED_COUNT; i++) {
        l3_rgb_set(i, r, g, b);
    }
}

static void l3_rgb_flush(void) {
    // Non-blocking: drained later from housekeeping_task_kb().
    l3_rgb_dirty = true;
}

const rgblight_driver_t rgblight_driver = {
    .init          = l3_rgb_init,
    .set_color     = l3_rgb_set,
    .set_color_all = l3_rgb_set_all,
    .flush         = l3_rgb_flush,
};
#endif

// --- single-level backlight (custom driver) ------------------------------
#ifdef BACKLIGHT_ENABLE
void backlight_init_ports(void) {
    // Companion-driven; no local pins to set up.
}

void backlight_set(uint8_t level) {
    l3_backlight_level = level;
    l3_backlight_dirty = true;
}
#endif

// --- lock LEDs ------------------------------------------------------------
bool led_update_kb(led_t led_state) {
    bool res = led_update_user(led_state);
    if (res) {
        uint8_t lock = 0;
        if (led_state.num_lock) {
            lock |= (1 << 2);
        }
        if (led_state.caps_lock) {
            lock |= (1 << 1);
        }
        if (led_state.scroll_lock) {
            lock |= (1 << 0);
        }
        l3_lock       = lock;
        l3_lock_dirty = true;
    }
    return res;
}

// --- keyboard hooks -------------------------------------------------------
void keyboard_pre_init_kb(void) {
    // Preserve the LeeKu port-D startup state before USB comes up. PD0/PD1 are
    // board control pins; PD4-PD7 keep pull-ups enabled. Matrix pins are on
    // PA/PB/PC and are owned by QMK.
    DDRD  = 0x03;
    PORTD = 0xF1;

    i2c_init();
    keyboard_pre_init_user();
}

void keyboard_post_init_kb(void) {
    l3_configure();
    l3_ready = true;
    keyboard_post_init_user();
}

void housekeeping_task_kb(void) {
    // Core calls housekeeping_task_user() separately (see quantum/keyboard.c),
    // so it must not be called here.
    l3_task();
}
