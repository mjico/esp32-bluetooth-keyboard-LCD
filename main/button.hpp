#pragma once

#include "button.h"

#include "esp_err.h"

/* KEY1..KEY4 live on XL9555 port 0, bits 4..7. Mask bit 1 means that key is pressed. */
class Button {
public:
    static const int kCount = 4;

    static const int kMaxBindings = 16;

    typedef void (*Action)(void);

    Button();

    /* Bind action to an exact key mask. KEY1 | KEY2 does not also run the KEY1 action. */
    bool bind(uint8_t mask, Action action);
    bool bind_long(uint8_t mask, Action action, int hold_ms);
    esp_err_t start(uint8_t addr);

    /* Modal list session. See button_choice_start. */
    bool choice_start(int item_count, button_choice_fn on_pick, button_choice_fn on_page, void *ctx);
    void choice_stop();
    bool choice_active();

private:
    struct Binding {
        uint8_t mask;
        Action action;
    };

    struct LongBinding {
        uint8_t mask;
        Action action;
        int hold_ms;
    };

    struct Choice {
        bool active;
        int item_count;
        int page;
        button_choice_fn on_pick;
        button_choice_fn on_page;
        void *ctx;
    };

    static void task_entry(void *arg);
    void run();
    bool read_pressed(uint8_t *pressed_mask);
    void show_released(uint8_t pressed_mask);
    void dispatch(uint8_t pressed_mask);
    LongBinding *find_long(uint8_t mask);
    void fill_choice(button_choice_t *out, int slot) const;
    /* HANDLED: session ate the key. YIELD: session ended, caller runs the normal binding. */
    int handle_choice(uint8_t pressed_mask);

    uint8_t addr_;
    uint8_t pressed_;
    bool have_sample_;
    static const int kMaxLong = 4;

    Binding bindings_[kMaxBindings];
    int binding_count_;
    LongBinding long_bindings_[kMaxLong];
    int long_count_;
    Choice choice_;
};
