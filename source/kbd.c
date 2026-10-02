#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "kbd.h"

bool kbd_prompt(char *buf, size_t buf_size, const char *hint,
                const char *confirm_text) {
    if (buf_size == 0) return false;

    SwkbdState swkbd;
    // 2 buttons: Cancel (left), Confirm (right). Cap input length at the
    // destination buffer size minus NUL (swkbd hard limit is ~2047 anyway).
    int max_len = (int)buf_size - 1;
    if (max_len > 1023) max_len = 1023;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, max_len);
    swkbdSetHintText(&swkbd, hint);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT,
                   confirm_text ? confirm_text : "OK", true);
    swkbdSetFeatures(&swkbd, SWKBD_PREDICTIVE_INPUT);
    swkbdSetInitialText(&swkbd, buf);

    char tmp[1024];
    tmp[0] = 0;

    SwkbdButton btn = swkbdInputText(&swkbd, tmp, sizeof(tmp));
    if (btn != SWKBD_BUTTON_RIGHT) return false;

    snprintf(buf, buf_size, "%s", tmp);
    return true;
}
