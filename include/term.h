#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <3ds.h>

// Top screen is 400x240 -> 50x30 text cells with the 8x8 console font.
#define TERM_COLS 50
#define TERM_ROWS 30
#define TERM_MAX_LINES 512
#define TERM_STATUS_ROW 0
#define TERM_BODY_TOP 1
#define TERM_BODY_ROWS (TERM_ROWS - 1)

typedef enum {
    LINE_SYS = 0,
    LINE_USER,
    LINE_ASSISTANT,
    LINE_ERROR,
    LINE_DIM,
} LineKind;

typedef struct {
    char lines[TERM_MAX_LINES][TERM_COLS + 1];
    LineKind kinds[TERM_MAX_LINES];
    int count;
    int scroll; // 0 = pinned to bottom
    bool dirty;
} TermBuffer;

void term_init(TermBuffer *tb);
void term_add(TermBuffer *tb, LineKind kind, const char *text);
void term_add_fmt(TermBuffer *tb, LineKind kind, const char *fmt, ...);
void term_scroll(TermBuffer *tb, int delta);
void term_scroll_to_bottom(TermBuffer *tb);
// Renders into the given console (select the top-screen console).
void term_render(TermBuffer *tb, PrintConsole *con, const char *status);
