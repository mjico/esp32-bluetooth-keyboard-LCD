#include "font.h"

#include "config.h"
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
    config_set_font(active_width);
}

static void cmd_font(int argc, char **argv)
{
    if (argc >= 1) {
        if ((argv[0][0] != '5' && argv[0][0] != '6') || argv[0][1] != '\0') {
            trace_print("system font [5|6]");
            return;
        }
        font_set_width(argv[0][0] - '0');
        lcd_text_redraw();
    }
    trace_printf("font %d", font_width());
    if (argc < 1) {
        trace_print("system font [5|6]");
    }
}

void font_command(int argc, char **argv)
{
    cmd_font(argc, argv);
}
