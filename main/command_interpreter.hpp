#pragma once

#include "command_interpreter.h"

/* One command is a name plus the following words. Handlers receive argv[0] as the name. */
class SerialCommand {
public:
    static const int kMaxCommands = 16;
    static const int kMaxArgs = 16;

    SerialCommand();

    bool add(const char *name, const char *help, command_fn_t action);
    bool execute(esp_lcd_panel_handle_t panel, char *line);
    void help(void) const;

private:
    struct Entry {
        const char *name;
        const char *help;
        command_fn_t action;
    };

    Entry table_[kMaxCommands];
    int count_;
};
