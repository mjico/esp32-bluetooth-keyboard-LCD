#include "font.h"

#include "command_interpreter.h"
#include "lcd_text.h"
#include "trace.h"

#include "font8x16.inc"
#include "font8x16_w6.inc"

static int active_width = 5;

const uint8_t *font_glyph(char ch)
{
    int index = 0;
    if (ch >= 32 && ch <= 126) {
        index = ch - 32;
    }
    if (active_width == 6) {
        return font8x16_w6[index];
    }
    return font8x16[index];
}

int font_width(void)
{
    return active_width;
}

void font_set_width(int width)
{
    if (width == 6) {
        active_width = 6;
    } else {
        active_width = 5;
    }
}

static void cmd_font(int argc, char **argv)
{
    if (argc >= 2) {
        if ((argv[1][0] != '5' && argv[1][0] != '6') || argv[1][1] != '\0') {
            trace_print("font [5|6]");
            return;
        }
        font_set_width(argv[1][0] - '0');
        lcd_text_redraw();
    }
    trace_printf("font %d", font_width());
}

void font_register_commands(void)
{
    command_register("font", "glyph width 5 or 6", cmd_font);
}
