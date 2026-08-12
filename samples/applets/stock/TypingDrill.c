/**
 * @file TypingDrill.c
 * @brief Short prompt typing test — WPM + accuracy after each run.
 *
 * Enter=start/again  type exactly  Backspace=undo
 * Esc=cancel  Clear File=reset best
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B5
#define PROMPT_MAX 36
#define PROMPT_COUNT 12

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Type Drill")
    APPLET_INFO("Timed typing WPM drill")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 64,
APPLET_HEADER_END

typedef enum {
    MODE_IDLE = 0,
    MODE_RUN = 1,
    MODE_DONE = 2
} drill_mode_t;

typedef struct {
    drill_mode_t mode;
    uint8_t prompt_i;
    uint8_t pos;
    uint16_t errors;
    uint32_t start_ms;
    uint16_t wpm;
    uint8_t accuracy;
    uint16_t elapsed_s;
    uint16_t best_wpm;
    uint8_t new_best;
    char typed[PROMPT_MAX];
} drill_state_t;

static drill_state_t s;

static const char k_prompts[PROMPT_COUNT][PROMPT_MAX] = {
    "the quick brown fox jumps",
    "pack my box with five dozen",
    "neo writes without a cable",
    "small keys and big ideas",
    "practice makes words faster",
    "type true then keep going",
    "focus on the next letter",
    "write often read even more",
    "quiet rooms make clear thought",
    "one word then another word",
    "keep your fingers light",
    "finish strong and smile",
};

static void save_best(void)
{
    char buf[20];
    char n[8];
    memset(buf, 0, sizeof(buf));
    stock_u32_to_str(s.best_wpm, n, sizeof(n));
    strncpy(buf, "B=", sizeof(buf) - 1);
    strncat(buf, n, sizeof(buf) - strlen(buf) - 1);
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
    if (FileOpen(1)) {
        FileWriteBuffer(buf, (uint16_t)sizeof(buf));
        FileClose();
    }
}

static void load_best(void)
{
    char buf[20];
    char *b;
    s.best_wpm = 0;
    memset(buf, 0, sizeof(buf));
    if (!FileOpen(1)) {
        return;
    }
    (void)FileReadBuffer(buf, (uint16_t)(sizeof(buf) - 1));
    FileClose();
    b = strstr(buf, "B=");
    if (b) {
        s.best_wpm = (uint16_t)stock_parse_u32(b + 2);
    }
}

static const char *cur_prompt(void)
{
    return k_prompts[s.prompt_i % PROMPT_COUNT];
}

static void finish_run(void)
{
    uint32_t now = GetUptimeMilliseconds();
    uint32_t ms = (now >= s.start_ms) ? (now - s.start_ms) : 1u;
    uint32_t secs = stock_udiv32(ms + 999u, 1000u);
    uint32_t plen = (uint32_t)strlen(cur_prompt());
    uint32_t total_keys;
    uint32_t acc;

    if (secs == 0) {
        secs = 1;
    }
    s.wpm = (uint16_t)stock_udiv32(plen * 12u, secs);
    s.elapsed_s = (uint16_t)secs;
    total_keys = plen + (uint32_t)s.errors;
    if (total_keys == 0) {
        total_keys = 1;
    }
    acc = stock_udiv32(plen * 100u, total_keys);
    s.accuracy = (acc > 100) ? 100 : (uint8_t)acc;
    s.new_best = 0;
    if (s.wpm > s.best_wpm) {
        s.best_wpm = s.wpm;
        s.new_best = 1;
        save_best();
    }
    s.mode = MODE_DONE;
}

static void start_run(void)
{
    s.prompt_i = (uint8_t)stock_urem16((uint16_t)(GetUptimeMilliseconds() & 0xFFFF), PROMPT_COUNT);
    s.pos = 0;
    s.errors = 0;
    s.start_ms = GetUptimeMilliseconds();
    s.wpm = 0;
    s.accuracy = 0;
    s.elapsed_s = 0;
    s.new_best = 0;
    memset(s.typed, 0, sizeof(s.typed));
    s.mode = MODE_RUN;
}

static void draw_idle(void)
{
    char line[41];
    char n[8];

    ClearScreen();
    SetCursorMode(CURSOR_MODE_HIDE);
    PutStringCentered(1, "Type Drill");
    PutStringCentered(3, "Match the prompt for WPM");
    if (s.best_wpm > 0) {
        stock_u32_to_str(s.best_wpm, n, sizeof(n));
        strncpy(line, "Best ", sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, n, sizeof(line) - strlen(line) - 1);
        strncat(line, " WPM", sizeof(line) - strlen(line) - 1);
        PutStringCentered(5, line);
    } else {
        PutStringCentered(5, "No best yet — beat the clock");
    }
    PutStringCentered(8, "Enter=start  Find=next prompt");
}

static void draw_run_status(void)
{
    char line[41];
    char n[8];
    uint32_t now = GetUptimeMilliseconds();
    uint32_t secs = stock_udiv32((now >= s.start_ms) ? (now - s.start_ms) : 0, 1000u);

    ClearRowCols(6, 1, 40);
    stock_u32_to_str((uint16_t)secs, n, sizeof(n));
    strncpy(line, n, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, "s  err ", sizeof(line) - strlen(line) - 1);
    stock_u32_to_str(s.errors, n, sizeof(n));
    strncat(line, n, sizeof(line) - strlen(line) - 1);
    PutStringCentered(6, line);
}

static void draw_run(void)
{
    ClearScreen();
    SetCursorMode(CURSOR_MODE_HIDE);
    PutStringCentered(1, "Type this:");
    PutStringCentered(2, cur_prompt());
    ClearRowCols(4, 1, 40);
    SetCursor(4, 1, CURSOR_MODE_SHOW);
    PutStringRaw(s.typed);
    draw_run_status();
    PutStringCentered(8, "Esc=cancel");
}

static void draw_typed_only(void)
{
    ClearRowCols(4, 1, 40);
    SetCursor(4, 1, CURSOR_MODE_SHOW);
    PutStringRaw(s.typed);
    draw_run_status();
}

static void draw_done(void)
{
    char line[41];
    char a[8], b[8], c[8];

    ClearScreen();
    SetCursorMode(CURSOR_MODE_HIDE);
    PutStringCentered(1, s.new_best ? "New best!" : "Results");
    stock_u32_to_str(s.wpm, a, sizeof(a));
    stock_u32_to_str(s.accuracy, b, sizeof(b));
    stock_u32_to_str(s.elapsed_s, c, sizeof(c));
    strncpy(line, a, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, " WPM  ", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, "%  ", sizeof(line) - strlen(line) - 1);
    strncat(line, c, sizeof(line) - strlen(line) - 1);
    strncat(line, "s", sizeof(line) - strlen(line) - 1);
    PutStringCentered(3, line);
    stock_u32_to_str(s.best_wpm, a, sizeof(a));
    strncpy(line, "Best ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    strncat(line, " WPM", sizeof(line) - strlen(line) - 1);
    PutStringCentered(5, line);
    PutStringCentered(8, "Enter = again");
}

static void draw(void)
{
    if (s.mode == MODE_RUN) {
        draw_run();
    } else if (s.mode == MODE_DONE) {
        draw_done();
    } else {
        draw_idle();
    }
}

static void on_char(char ch)
{
    const char *prompt;
    size_t plen;

    if (s.mode != MODE_RUN) {
        return;
    }
    if (ch < 0x20 || ch > 0x7e) {
        return;
    }
    prompt = cur_prompt();
    plen = strlen(prompt);
    if (s.pos >= plen || s.pos >= PROMPT_MAX - 1) {
        return;
    }
    if (ch == prompt[s.pos]) {
        s.typed[s.pos] = ch;
        s.pos++;
        s.typed[s.pos] = '\0';
        if (s.pos >= plen) {
            finish_run();
            draw();
        } else {
            draw_typed_only();
        }
    } else {
        if (s.errors < 9999) {
            s.errors++;
        }
        draw_run_status();
    }
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.mode = MODE_IDLE;
        break;
    case MSG_SETFOCUS:
        load_best();
        if (s.mode == MODE_RUN) {
            s.mode = MODE_IDLE;
        }
        draw();
        break;
    case MSG_KILLFOCUS:
        break;
    case MSG_CHAR:
        /* Prefer MSG_CHAR only — avoid double-count with MSG_KEY. */
        on_char((char)(param & 0xFF));
        break;
    case MSG_KEY: {
        KeyMod_e key = (KeyMod_e)param;
        uint8_t code = (uint8_t)(key & 0xFF);

        if (code == KEY_ENTER || code == KEY_SEND) {
            if (s.mode == MODE_IDLE || s.mode == MODE_DONE) {
                start_run();
                draw();
            }
            break;
        }
        if (code == KEY_FIND) {
            if (s.mode == MODE_IDLE || s.mode == MODE_DONE) {
                s.prompt_i = (uint8_t)stock_urem16((uint16_t)(s.prompt_i + 1), PROMPT_COUNT);
                ClearScreen();
                SetCursorMode(CURSOR_MODE_HIDE);
                PutStringCentered(1, "Next prompt");
                PutStringCentered(3, cur_prompt());
                PutStringCentered(8, "Enter=start  Find=next");
            }
            break;
        }
        if (code == KEY_ESC) {
            if (s.mode == MODE_RUN) {
                s.mode = MODE_IDLE;
                draw();
            }
            break;
        }
        if (code == KEY_CLEAR_FILE) {
            s.best_wpm = 0;
            save_best();
            s.mode = MODE_IDLE;
            draw();
            break;
        }
        if (s.mode == MODE_RUN && (code == KEY_BACKSPACE || code == KEY_DELETE)) {
            if (s.pos > 0) {
                s.pos--;
                s.typed[s.pos] = '\0';
                draw_typed_only();
            }
            break;
        }
        break;
    }
    default:
        break;
    }
}
