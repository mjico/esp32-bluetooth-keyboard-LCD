#pragma once

#include "command_interpreter.h"
#include "lcd_text.h"

/* One command is a name plus the following words. Handlers receive argv[0] as the name. */
class SerialCommand {
public:
    /* help prints one line per command. The last row is the prompt. */
    static const int kMaxCommands = LCD_TEXT_ROWS - 1;
    static const int kMaxArgs = 16;

    SerialCommand();

    bool add(const char *name, const char *help, command_fn_t action);
    bool execute(char *line);
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
