#pragma once

#include <stdbool.h>
#include <stddef.h>

// Open the 3DS software keyboard (swkbd) applet to edit `buf`.
// `buf` is used as the initial text and receives the result.
// Returns true if the user pressed the confirm (right) button.
bool kbd_prompt(char *buf, size_t buf_size, const char *hint,
                const char *confirm_text);
