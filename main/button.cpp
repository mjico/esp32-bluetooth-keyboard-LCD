#include "button.hpp"

#include <stdio.h>
#include <string.h>

#include "button.h"
#include "i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_text.h"
#include "trace.h"

static const uint8_t kRegInput0 = 0x00;
static const uint8_t kRegConfig0 = 0x06;
static const uint8_t kKeyBits = 0xF0;
static const int kChoiceHandled = 1;
static const int kChoiceYield = 2;

static portMUX_TYPE choice_mux = portMUX_INITIALIZER_UNLOCKED;

Button::Button()
    : addr_(0), pressed_(0), have_sample_(false), binding_count_(0), long_count_(0), choice_()
{
    memset(bindings_, 0, sizeof(bindings_));
    memset(long_bindings_, 0, sizeof(long_bindings_));
}

bool Button::bind(uint8_t mask, Action action)
{
    if (mask == 0 || action == NULL) {
        return false;
    }
    for (int i = 0; i < binding_count_; i++) {
        if (bindings_[i].mask == mask) {
            bindings_[i].action = action;
            return true;
        }
    }
    if (binding_count_ >= kMaxBindings) {
        return false;
    }
    bindings_[binding_count_].mask = mask;
    bindings_[binding_count_].action = action;
    binding_count_++;
    return true;
}

bool Button::bind_long(uint8_t mask, Action action, int hold_ms)
{
    if (mask == 0 || action == NULL || hold_ms <= 0) {
        return false;
    }
    for (int i = 0; i < long_count_; i++) {
        if (long_bindings_[i].mask == mask) {
            long_bindings_[i].action = action;
            long_bindings_[i].hold_ms = hold_ms;
            return true;
        }
    }
    if (long_count_ >= kMaxLong) {
        return false;
    }
    long_bindings_[long_count_].mask = mask;
    long_bindings_[long_count_].action = action;
    long_bindings_[long_count_].hold_ms = hold_ms;
    long_count_++;
    return true;
}

Button::LongBinding *Button::find_long(uint8_t mask)
{
    for (int i = 0; i < long_count_; i++) {
        if (long_bindings_[i].mask == mask) {
            return &long_bindings_[i];
        }
    }
    return NULL;
}

esp_err_t Button::start(uint8_t addr)
{
    uint8_t dir = 0xFF;
    esp_err_t err = i2c_reg_read(addr, kRegConfig0, &dir, 1);
    if (err == ESP_OK) {
        dir = (uint8_t)(dir | kKeyBits);
        err = i2c_reg_write(addr, kRegConfig0, &dir, 1);
    }
    if (err != ESP_OK) {
        return err;
    }

    addr_ = addr;
    if (xTaskCreate(task_entry, "button", 8192, this, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool Button::choice_start(int item_count, button_choice_fn on_pick, button_choice_fn on_page, void *ctx)
{
    if (item_count < 1 || on_pick == NULL) {
        return false;
    }

    button_choice_t choice;
    portENTER_CRITICAL(&choice_mux);
    choice_.active = true;
    choice_.item_count = item_count;
    choice_.page = 0;
    choice_.on_pick = on_pick;
    choice_.on_page = on_page;
    choice_.ctx = ctx;
    fill_choice(&choice, -1);
    portEXIT_CRITICAL(&choice_mux);

    if (on_page != NULL) {
        on_page(&choice, ctx);
    }
    return true;
}

void Button::choice_stop()
{
    portENTER_CRITICAL(&choice_mux);
    choice_.active = false;
    portEXIT_CRITICAL(&choice_mux);
}

bool Button::choice_active()
{
    portENTER_CRITICAL(&choice_mux);
    bool active = choice_.active;
    portEXIT_CRITICAL(&choice_mux);
    return active;
}

void Button::fill_choice(button_choice_t *out, int slot) const
{
    int page_count = (choice_.item_count + BUTTON_CHOICE_SLOTS - 1) / BUTTON_CHOICE_SLOTS;
    if (page_count < 1) {
        page_count = 1;
    }
    int index = -1;
    if (slot >= 0) {
        index = choice_.page * BUTTON_CHOICE_SLOTS + slot;
        if (index >= choice_.item_count) {
            index = -1;
        }
    }
    out->index = index;
    out->slot = slot;
    out->page = choice_.page;
    out->page_count = page_count;
    out->item_count = choice_.item_count;
}

int Button::handle_choice(uint8_t pressed_mask)
{
    button_choice_fn on_pick = NULL;
    button_choice_fn on_page = NULL;
    void *ctx = NULL;
    button_choice_t choice;
    bool call_pick = false;
    bool call_page = false;
    int slot = -1;

    if (pressed_mask == KEY1) {
        slot = 0;
    } else if (pressed_mask == KEY2) {
        slot = 1;
    } else if (pressed_mask == KEY3) {
        slot = 2;
    }

    portENTER_CRITICAL(&choice_mux);
    if (!choice_.active) {
        portEXIT_CRITICAL(&choice_mux);
        return kChoiceYield;
    }
    if (slot >= 0) {
        fill_choice(&choice, slot);
        if (choice.index >= 0) {
            on_pick = choice_.on_pick;
            ctx = choice_.ctx;
            choice_.active = false;
            call_pick = true;
        }
        portEXIT_CRITICAL(&choice_mux);
        if (call_pick && on_pick != NULL) {
            on_pick(&choice, ctx);
        }
        return kChoiceHandled;
    }
    if (pressed_mask == KEY4) {
        int page_count = (choice_.item_count + BUTTON_CHOICE_SLOTS - 1) / BUTTON_CHOICE_SLOTS;
        if (page_count < 1) {
            page_count = 1;
        }
        choice_.page = (choice_.page + 1) % page_count;
        fill_choice(&choice, -1);
        on_page = choice_.on_page;
        ctx = choice_.ctx;
        call_page = true;
        portEXIT_CRITICAL(&choice_mux);
        if (call_page && on_page != NULL) {
            on_page(&choice, ctx);
        }
        return kChoiceHandled;
    }
    choice_.active = false;
    portEXIT_CRITICAL(&choice_mux);
    return kChoiceYield;
}

void Button::task_entry(void *arg)
{
    static_cast<Button *>(arg)->run();
}

bool Button::read_pressed(uint8_t *pressed_mask)
{
    uint8_t value = 0;
    if (i2c_reg_read(addr_, kRegInput0, &value, 1) != ESP_OK) {
        return false;
    }
    /* IO0_4..IO0_7 are KEY1..KEY4. A pressed key pulls its pin low, so invert before shifting. */
    *pressed_mask = (uint8_t)(((~value) & kKeyBits) >> 4);
    return true;
}

void Button::show_released(uint8_t pressed_mask)
{
    if (have_sample_ && pressed_mask == pressed_) {
        return;
    }
    have_sample_ = true;
    pressed_ = pressed_mask;

    /* Same bits as dispatch. Name the keys that are held, not the ones left up. */
    if (pressed_mask == 0) {
        return;
    }

    static const char *names[kCount] = {"KEY1", "KEY2", "KEY3", "KEY4"};
    char line[LCD_TEXT_COLS + 1];
    int used = snprintf(line, sizeof(line), "up");
    for (int i = 0; i < kCount && used > 0 && used < (int)sizeof(line); i++) {
        if ((pressed_mask & (1 << i)) != 0) {
            used += snprintf(line + used, sizeof(line) - used, " %s", names[i]);
        }
    }
    trace_print(line);
}

void Button::dispatch(uint8_t pressed_mask)
{
    for (int i = 0; i < binding_count_; i++) {
        if (bindings_[i].mask == pressed_mask && bindings_[i].action != NULL) {
            bindings_[i].action();
            return;
        }
    }
}

void Button::run()
{
    uint8_t pending = 0;
    bool have_pending = false;
    uint8_t chord = 0;
    uint8_t fired = 0;
    TickType_t chord_tick = 0;

    for (;;) {
        uint8_t pressed = 0;
        if (!read_pressed(&pressed)) {
            trace_print("key read failed");
            vTaskDelay(pdMS_TO_TICKS(100));
            have_pending = false;
            continue;
        }
        if (!have_pending || pressed != pending) {
            pending = pressed;
            have_pending = true;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool choosing = choice_active();
        if (choosing) {
            /* Keep the released-key line in sync, but do not print over the open list. */
            have_sample_ = true;
            pressed_ = pressed;
        } else {
            show_released(pressed);
        }
        if (pressed == 0) {
            LongBinding *held_long = find_long(chord);
            if (!choosing && chord != 0 && chord != fired && held_long != NULL) {
                dispatch(chord);
            }
            fired = 0;
            chord = 0;
        } else if (pressed != fired) {
            if (pressed != chord) {
                chord = pressed;
                chord_tick = xTaskGetTickCount();
            } else {
                LongBinding *held_long = choosing ? NULL : find_long(pressed);
                int need_ms = held_long != NULL ? held_long->hold_ms : 150;
                if ((xTaskGetTickCount() - chord_tick) >= pdMS_TO_TICKS(need_ms)) {
                    if (choosing) {
                        if (handle_choice(pressed) == kChoiceYield) {
                            dispatch(pressed);
                        }
                    } else if (held_long != NULL) {
                        held_long->action();
                    } else {
                        dispatch(pressed);
                    }
                    fired = pressed;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static Button &buttons(void)
{
    static Button buttons;
    return buttons;
}

extern "C" esp_err_t button_bind(uint8_t mask, button_action_t action)
{
    return buttons().bind(mask, action) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

extern "C" esp_err_t button_bind_long(uint8_t mask, button_action_t action, int hold_ms)
{
    return buttons().bind_long(mask, action, hold_ms) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

extern "C" esp_err_t button_start(uint8_t addr)
{
    return buttons().start(addr);
}

extern "C" esp_err_t button_choice_start(int item_count, button_choice_fn on_pick, button_choice_fn on_page, void *ctx)
{
    return buttons().choice_start(item_count, on_pick, on_page, ctx) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

extern "C" void button_choice_stop(void)
{
    buttons().choice_stop();
}

extern "C" bool button_choice_active(void)
{
    return buttons().choice_active();
}
