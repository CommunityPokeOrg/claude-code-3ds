#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>

#include "term.h"
#include "settings.h"
#include "kbd.h"
#include "net.h"

#define COMPOSE_MAX 900
#define BOT_COLS 40 // bottom screen is 320x240 -> 40x30 cells

// SOC service shared-memory buffer (must be 0x1000-aligned).
#define SOC_BUFFER_SIZE 0x100000

static u32 *g_soc_buffer = NULL;

// Bounded string copy that won't trip -Wformat-truncation.
static void str_copy(char *dst, size_t dst_size, const char *src) {
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

typedef enum {
    SCREEN_CHAT = 0,
    SCREEN_SETTINGS,
    SCREEN_QUIT,
} Screen;

static PrintConsole g_top;
static PrintConsole g_bottom;

static TermBuffer g_term;
static Settings g_settings;
static Conversation g_conv;
static NetJob g_job;
static char g_compose[COMPOSE_MAX + 1];
static char g_status[128];
static char g_hints[128];
static int g_frame = 0;
static bool g_bottom_dirty = true;

static const char *spinner(int frame) {
    static const char *frames[] = {"|", "/", "-", "\\"};
    return frames[(frame / 6) % 4];
}

static void send_message(const char *text) {
    if (!text || !*text) return;
    if (g_settings.api_key[0] == 0) {
        term_add(&g_term, LINE_ERROR,
                 "No API key set. Press SELECT to open Settings.");
        return;
    }
    conv_add(&g_conv, "user", text);
    term_add_fmt(&g_term, LINE_USER, "> %s", text);
    if (!net_send_async(&g_job, &g_conv, &g_settings)) {
        term_add(&g_term, LINE_ERROR, "Could not start request (busy?)");
        conv_pop(&g_conv);
    }
    term_scroll_to_bottom(&g_term);
}

static void update_status(void) {
    ReqState st = net_poll(&g_job);
    const char *net;
    switch (st) {
        case REQ_RUNNING:   net = "sending..."; break;
        case REQ_IDLE:
        default:            net = "ready"; break;
    }
    const char *scroll_note = g_term.scroll ? " [scrolled]" : "";
    snprintf(g_status, sizeof(g_status),
             " CC3DS | %s | %s%s",
             g_settings.model, net, scroll_note);
    if (st == REQ_RUNNING)
        snprintf(g_hints, sizeof(g_hints),
                 " %s thinking | B cancel | SEL settings | START quit",
                 spinner(g_frame));
    else
        snprintf(g_hints, sizeof(g_hints),
                 " A compose | dpad scroll | X clear | SEL settings | START quit");
}

static void handle_request_done(void) {
    ReqState st = net_poll(&g_job);
    switch (st) {
        case REQ_DONE:
            conv_add(&g_conv, "assistant", g_job.result);
            term_add(&g_term, LINE_ASSISTANT, g_job.result);
            break;
        case REQ_ERROR:
            term_add_fmt(&g_term, LINE_ERROR, "Error: %s", g_job.error);
            conv_pop(&g_conv);
            break;
        case REQ_CANCELLED:
            term_add(&g_term, LINE_DIM, "(request cancelled)");
            conv_pop(&g_conv);
            break;
        default:
            return;
    }
    net_finish(&g_job);
    term_scroll_to_bottom(&g_term);
}

// ---------------------------------------------------------------
// Bottom screen: compose box + hint bar
// ---------------------------------------------------------------

static void render_bottom(void) {
    consoleSelect(&g_bottom);
    consoleClear();

    // Compose header.
    printf("\x1b[7m%-*s\x1b[0m\n", BOT_COLS, " Compose");

    // Draft, wrapped to the bottom screen width. Only the tail that fits is
    // shown (22 rows max); a long draft scrolls off the top.
    char lines[24][BOT_COLS + 1];
    int wrows = 0;
    if (!g_compose[0]) {
        snprintf(lines[0], sizeof(lines[0]), "(empty - press A to type)");
        wrows = 1;
    } else {
        const char *p = g_compose;
        while (*p) {
            if (wrows == 24) {
                // Drop the oldest row to keep the tail visible.
                memmove(lines[0], lines[1], 23 * sizeof(lines[0]));
                wrows--;
            }
            int n = 0;
            while (*p && *p != '\n' && n < BOT_COLS)
                lines[wrows][n++] = *p++;
            lines[wrows][n] = 0;
            wrows++;
            if (*p == '\n') p++;
        }
    }
    int first = wrows > 22 ? wrows - 22 : 0;
    for (int i = first; i < wrows; i++)
        printf("\x1b[32m%s\x1b[0m\n", lines[i]);

    // Push the hint bar to the bottom of the screen.
    int printed = 1 + (wrows - first);
    for (int i = printed; i < 29; i++) printf("\n");
    printf("\x1b[7m%-*s\x1b[0m", BOT_COLS, g_hints);
}

static void chat_screen(u32 down, u32 held, Screen *screen) {
    if (down & KEY_START)  { *screen = SCREEN_QUIT; return; }
    if (down & KEY_SELECT) { *screen = SCREEN_SETTINGS; return; }

    // Scrollback.
    if (down & KEY_L)
        term_scroll(&g_term, TERM_BODY_ROWS);
    else if (down & KEY_R)
        term_scroll(&g_term, -TERM_BODY_ROWS);
    else if (held & KEY_UP)
        term_scroll(&g_term, 1);
    else if (held & KEY_DOWN)
        term_scroll(&g_term, -1);

    if (down & KEY_B && net_poll(&g_job) == REQ_RUNNING) {
        net_cancel(&g_job);
        term_add(&g_term, LINE_DIM, "(cancelling...)");
    }

    if (down & KEY_X) {
        conv_clear(&g_conv);
        term_init(&g_term);
        term_add(&g_term, LINE_SYS, "Conversation cleared.");
    }

    if (down & KEY_A) {
        char draft[1024];
        str_copy(draft, sizeof(draft), g_compose);
        if (kbd_prompt(draft, sizeof(draft), "Message to Claude", "Send")) {
            if (net_poll(&g_job) == REQ_RUNNING) {
                str_copy(g_compose, sizeof(g_compose), draft);
                term_add(&g_term, LINE_DIM,
                         "(kept as draft - request still in flight)");
            } else {
                g_compose[0] = 0;
                send_message(draft);
            }
        } else {
            // Cancelled: swkbd wrote nothing, draft stays as-is.
        }
        g_bottom_dirty = true;
    }

    // Also allow sending the stored draft with Y (after a cancelled send or
    // a draft left over from a busy request).
    if (down & KEY_Y && g_compose[0] &&
        net_poll(&g_job) != REQ_RUNNING) {
        send_message(g_compose);
        g_compose[0] = 0;
        g_bottom_dirty = true;
    }

    ReqState st = net_poll(&g_job);
    if (st == REQ_DONE || st == REQ_ERROR || st == REQ_CANCELLED)
        handle_request_done();

    update_status();
    term_render(&g_term, &g_top, g_status);
    render_bottom();
}

// ---------------------------------------------------------------
// Settings screen (bottom screen)
// ---------------------------------------------------------------

typedef enum {
    SET_API_KEY = 0,
    SET_MODEL,
    SET_BASE_URL,
    SET_SAVE_BACK,
    SET_BACK,
    SET_COUNT,
} SettingItem;

static const char *setting_names[SET_COUNT] = {
    "API key",
    "Model",
    "Base URL",
    "Save & back",
    "Back (discard)",
};

static void mask_key(const char *key, char *out, size_t out_size) {
    size_t len = strlen(key);
    if (len == 0) { snprintf(out, out_size, "(not set)"); return; }
    if (len <= 8) { snprintf(out, out_size, "********"); return; }
    snprintf(out, out_size, "%.6s...%s (len %u)", key, key + len - 4,
             (unsigned)len);
}

static void settings_screen(u32 down, Screen *screen) {
    static int sel = 0;
    static Settings edit;
    static bool loaded = false;

    if (!loaded) {
        edit = g_settings;
        loaded = true;
    }

    if (down & KEY_UP)   sel = (sel + SET_COUNT - 1) % SET_COUNT;
    if (down & KEY_DOWN) sel = (sel + 1) % SET_COUNT;

    if (down & (KEY_B | KEY_START | KEY_SELECT)) {
        loaded = false;
        *screen = SCREEN_CHAT;
        return;
    }

    if (down & KEY_A) {
        switch (sel) {
            case SET_API_KEY:
                if (kbd_prompt(edit.api_key, sizeof(edit.api_key),
                               "Anthropic API key", "OK"))
                    g_bottom_dirty = true;
                break;
            case SET_MODEL:
                kbd_prompt(edit.model, sizeof(edit.model),
                           "Model", "OK");
                break;
            case SET_BASE_URL:
                kbd_prompt(edit.base_url, sizeof(edit.base_url),
                           "Base URL", "OK");
                break;
            case SET_SAVE_BACK:
                g_settings = edit;
                if (settings_save(&g_settings))
                    term_add(&g_term, LINE_SYS, "Settings saved to SD card.");
                else
                    term_add(&g_term, LINE_ERROR,
                             "Failed to write settings to SD card!");
                loaded = false;
                *screen = SCREEN_CHAT;
                return;
            case SET_BACK:
                loaded = false;
                *screen = SCREEN_CHAT;
                return;
            default:
                break;
        }
    }

    // Settings live on the bottom screen; transcript stays on top.
    consoleSelect(&g_bottom);
    consoleClear();
    printf("\x1b[7m%-*s\x1b[0m\n\n", BOT_COLS, " Settings");
    printf("\x1b[90msdmc:/config/claude-code-3ds/\x1b[0m\n\n");

    char masked[64];
    mask_key(edit.api_key, masked, sizeof(masked));
    const char *values[SET_COUNT] = {
        masked, edit.model, edit.base_url, "", "",
    };
    for (int i = 0; i < SET_COUNT; i++) {
        const char *cursor = (i == sel) ? " > " : "   ";
        if (values[i][0])
            printf("%s%s: %.26s\n", cursor, setting_names[i], values[i]);
        else
            printf("%s%s\n", cursor, setting_names[i]);
    }
    printf("\n\x1b[31m");
    printf("API key is stored in PLAINTEXT\n");
    printf("on the SD card. Anyone with SD\n");
    printf("access can read it. Use a key\n");
    printf("with a tight spend limit.\n");
    printf("\x1b[0m\n");
    for (int i = 0; i < 5; i++) printf("\n");
    printf("\x1b[7m%-*s\x1b[0m", BOT_COLS,
           " A edit | B/SEL back");
}

// ---------------------------------------------------------------

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    gfxInitDefault();
    consoleInit(GFX_TOP, &g_top);
    consoleInit(GFX_BOTTOM, &g_bottom);

    term_init(&g_term);
    term_add(&g_term, LINE_SYS,
             "Claude Code 3DS - terminal client for the Anthropic API");
    term_add(&g_term, LINE_DIM, "devkitPro/libctru build " __DATE__);

    Result rc = romfsInit();
    if (R_FAILED(rc))
        term_add_fmt(&g_term, LINE_ERROR,
                     "romfsInit failed (0x%lx) - TLS CA bundle missing, "
                     "HTTPS will fail", (unsigned long)rc);

    g_soc_buffer = (u32 *)memalign(0x1000, SOC_BUFFER_SIZE);
    if (!g_soc_buffer || R_FAILED(rc = socInit(g_soc_buffer, SOC_BUFFER_SIZE)))
        term_add_fmt(&g_term, LINE_ERROR,
                     "socInit failed (0x%lx) - no network",
                     (unsigned long)rc);

    settings_defaults(&g_settings);
    bool had_settings = settings_load(&g_settings);
    conv_init(&g_conv);

    if (!had_settings || g_settings.api_key[0] == 0) {
        term_add(&g_term, LINE_ERROR, "No API key configured yet.");
        term_add(&g_term, LINE_SYS,
                 "Press SELECT to open Settings and enter your key.");
    }

    update_status();
    Screen screen = SCREEN_CHAT;

    while (aptMainLoop() && screen != SCREEN_QUIT) {
        hidScanInput();
        u32 down = hidKeysDown();
        u32 held = hidKeysHeld();
        g_frame++;

        switch (screen) {
            case SCREEN_CHAT:     chat_screen(down, held, &screen); break;
            case SCREEN_SETTINGS: settings_screen(down, &screen); break;
            default: break;
        }

        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    if (net_poll(&g_job) == REQ_RUNNING) {
        net_cancel(&g_job);
        net_finish(&g_job);
    }

    conv_free(&g_conv);
    socExit();
    free(g_soc_buffer);
    romfsExit();
    gfxExit();
    return 0;
}
