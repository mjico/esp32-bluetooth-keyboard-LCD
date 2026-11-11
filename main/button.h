#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One bit per key. Combine with bitwise OR: KEY1 | KEY2. */
#define KEY1 (1u << 0)
#define KEY2 (1u << 1)
#define KEY3 (1u << 2)
#define KEY4 (1u << 3)

typedef void (*button_action_t)(esp_lcd_panel_handle_t panel);

/* Register the function that runs when exactly these keys are held. mask 0 is rejected. */
esp_err_t button_bind(uint8_t mask, button_action_t action);

/* If mask is held for hold_ms, action runs once and the normal binding does not.
 * A shorter press runs the normal binding on release. Choice sessions ignore this. */
esp_err_t button_bind_long(uint8_t mask, button_action_t action, int hold_ms);

/* Read KEY1..KEY4 from the shared I2C bus. */
esp_err_t button_start(uint8_t addr, esp_lcd_panel_handle_t panel);

/* One page has one slot per select key: KEY1, KEY2, KEY3. KEY4 cycles pages. */
#define BUTTON_CHOICE_SLOTS 3

typedef struct {
    int index;      /* 0-based item, or -1 when this call is a page change or an empty slot */
    int slot;       /* 0..2 for KEY1..KEY3, or -1 on a page change */
    int page;       /* 0-based page */
    int page_count;
    int item_count;
} button_choice_t;

typedef void (*button_choice_fn)(esp_lcd_panel_handle_t panel, const button_choice_t *choice, void *ctx);

/* Take over KEY1..KEY4 until a slot is chosen or button_choice_stop runs.
 * KEY1..KEY3 report that slot. An empty slot does nothing. KEY4 advances the page and wraps.
 * Holding any combination of two or more keys ends the session and runs the normal binding.
 * on_page may be NULL. It is called once immediately so the caller can draw page 0.
 */
esp_err_t button_choice_start(int item_count, button_choice_fn on_pick, button_choice_fn on_page, void *ctx);
void button_choice_stop(void);
bool button_choice_active(void);

#ifdef __cplusplus
}
#endif
