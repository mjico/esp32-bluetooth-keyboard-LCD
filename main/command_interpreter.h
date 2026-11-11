#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*command_fn_t)(esp_lcd_panel_handle_t panel, int argc, char **argv);

/* Register one command. keyword and help must stay valid. help is one short ASCII line. */
esp_err_t command_register(const char *keyword, const char *help, command_fn_t fn);

/* Register the built-in help command. Call once before other modules register. */
void command_init(void);

/* Split line into "command arg arg..." and run the matching command. */
void command_line_execute(esp_lcd_panel_handle_t panel, const char *line);

#ifdef __cplusplus
}
#endif
