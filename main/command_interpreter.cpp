#include "command_interpreter.hpp"

#include <string.h>

#include "command_history.h"
#include "trace.h"

SerialCommand::SerialCommand() : count_(0)
{
    memset(table_, 0, sizeof(table_));
}

bool SerialCommand::add(const char *name, const char *help, command_fn_t action)
{
    if (name == NULL || help == NULL || action == NULL || count_ >= kMaxCommands) {
        return false;
    }
    for (int i = 0; i < count_; i++) {
        if (strcmp(table_[i].name, name) == 0) {
            return false;
        }
    }
    table_[count_].name = name;
    table_[count_].help = help;
    table_[count_].action = action;
    count_++;
    return true;
}

bool SerialCommand::execute(char *line)
{
    char *argv[kMaxArgs];
    int argc = 0;
    char *cursor = line;

    if (line == NULL) {
        return false;
    }

    while (*cursor != '\0' && argc < kMaxArgs) {
        while (*cursor == ' ' || *cursor == '\t') {
            cursor++;
        }
        if (*cursor == '\0') {
            break;
        }
        argv[argc++] = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t') {
            cursor++;
        }
        if (*cursor != '\0') {
            *cursor = '\0';
            cursor++;
        }
    }

    if (argc == 0) {
        return false;
    }

    for (int i = 0; i < count_; i++) {
        if (strcmp(table_[i].name, argv[0]) == 0) {
            table_[i].action(argc, argv);
            return true;
        }
    }
    return false;
}

void SerialCommand::help(void) const
{
    for (int i = 0; i < count_; i++) {
        trace_printf("%s  %s", table_[i].name, table_[i].help);
    }
}

static SerialCommand &commands(void)
{
    static SerialCommand interpreter;
    return interpreter;
}

static void cmd_help(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    commands().help();
}

extern "C" void command_init(void)
{
    command_register("help", "list commands", cmd_help);
}

extern "C" esp_err_t command_register(const char *keyword, const char *help, command_fn_t fn)
{
    return commands().add(keyword, help, fn) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

extern "C" void command_line_execute(const char *line)
{
    char buffer[129];
    size_t length = 0;

    if (line == NULL) {
        return;
    }
    while (line[length] != '\0' && length < sizeof(buffer) - 1) {
        buffer[length] = line[length];
        length++;
    }
    buffer[length] = '\0';
    if (length == 0) {
        return;
    }
    command_history_add(line);
    if (!commands().execute(buffer)) {
        trace_print("unknown command");
    }
}
