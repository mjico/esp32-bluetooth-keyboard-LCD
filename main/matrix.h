#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void matrix_register_commands(void);

/* Draw one typed character when matrix key echo is on. Default is off. */
void matrix_on_key(char ch);

#ifdef __cplusplus
}
#endif
