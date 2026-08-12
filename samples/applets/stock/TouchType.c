/**
 * @file TouchType.c
 * @brief Touch-typing coach — lessons, live feedback, finger hints.
 *
 * Tab / Left-Right = pick lesson
 * Enter = start / next exercise
 * Type the line (wrong keys don't advance — learn, don't race)
 * Esc = menu   Find = skip exercise   Clear File = reset best
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1BB
#define EX_LEN 36
#define LESSON_COUNT 5
#define EX_PER 4
#define MODE_MENU 0
#define MODE_RUN 1
#define MODE_DONE 2

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Touch Type")
    APPLET_INFO("Learn touch typing with live feedback")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 64,
APPLET_HEADER_END

typedef struct {
    uint8_t mode;
    uint8_t lesson;
    uint8_t ex;
    uint8_t pos;
    uint16_t errors;
    uint16_t hits;
    uint32_t start_ms;
    uint16_t wpm;
    uint8_t accuracy;
    uint16_t best_wpm;
    uint8_t new_best;
    uint8_t flash_wrong;
    char typed[EX_LEN];
    char hint[18];
} tt_state_t;

static tt_state_t s;

static const char k_lesson[LESSON_COUNT][10] = {
    "1 Home", "2 Left", "3 Right", "4 Words", "5 Mix"
};

static const char k_guide[LESSON_COUNT][28] = {
    "asdf jkl; — home anchors",
    "qwer + zxcv left reach",
    "uiop + nm,. right reach",
    "short words, home first",
    "mixed rows + punctuation"
};

/* Flat tables only — no pointer arrays (m68k gc-sections). */
static const char k_ex[LESSON_COUNT][EX_PER][EX_LEN] = {
    { /* Home */
        "asdf jkl; asdf jkl;",
        "aaa sss ddd fff jjj",
        "fdsa jkl; lkj; asdf",
        "ask dad; fall; salad"
    },
    { /* Left reach */
        "aqa sws ded frf aqa",
        "aza sxs dcd fvf aza",
        "qwer asdf qwer asdf",
        "read a safe dew; far"
    },
    { /* Right reach */
        "juj kik lol ;p; juj",
        "jmj knk l,l ;.; jmj",
        "uiop jkl; uiop jkl;",
        "jump look; milk; oil"
    },
    { /* Words */
        "a lad asks; a flask",
        "dad falls; all ask",
        "salad; flask; glass",
        "just look; make milk"
    },
    { /* Mix */
        "the quick fox; pack",
        "type true; keep calm",
        "focus; practice daily",
        "neo keys; soft hands"
    }
};

static const char *cur_ex(void)
{
    return k_ex[s.lesson][s.ex];
}

static void finger_hint(char c)
{
    /* Compact hand+finger tip for the next key. */
    if (c == ' ') {
        strncpy(s.hint, "thumbs  space", sizeof(s.hint) - 1);
    } else if (c == 'a' || c == 'q' || c == 'z' || c == 'A' || c == 'Q' || c == 'Z') {
        strncpy(s.hint, "L pinky", sizeof(s.hint) - 1);
    } else if (c == 's' || c == 'w' || c == 'x' || c == 'S' || c == 'W' || c == 'X') {
        strncpy(s.hint, "L ring", sizeof(s.hint) - 1);
    } else if (c == 'd' || c == 'e' || c == 'c' || c == 'D' || c == 'E' || c == 'C') {
        strncpy(s.hint, "L middle", sizeof(s.hint) - 1);
    } else if (c == 'f' || c == 'r' || c == 'v' || c == 'g' || c == 't' || c == 'b' ||
               c == 'F' || c == 'R' || c == 'V' || c == 'G' || c == 'T' || c == 'B') {
        strncpy(s.hint, "L index", sizeof(s.hint) - 1);
    } else if (c == 'j' || c == 'u' || c == 'm' || c == 'h' || c == 'y' || c == 'n' ||
               c == 'J' || c == 'U' || c == 'M' || c == 'H' || c == 'Y' || c == 'N') {
        strncpy(s.hint, "R index", sizeof(s.hint) - 1);
    } else if (c == 'k' || c == 'i' || c == ',' || c == 'K' || c == 'I' || c == '<') {
        strncpy(s.hint, "R middle", sizeof(s.hint) - 1);
    } else if (c == 'l' || c == 'o' || c == '.' || c == 'L' || c == 'O' || c == '>') {
        strncpy(s.hint, "R ring", sizeof(s.hint) - 1);
    } else if (c == ';' || c == 'p' || c == '/' || c == ':' || c == 'P' || c == '?') {
        strncpy(s.hint, "R pinky", sizeof(s.hint) - 1);
    } else {
        strncpy(s.hint, "home hands", sizeof(s.hint) - 1);
    }
    s.hint[sizeof(s.hint) - 1] = '\0';
}

static void load_best(void)
{
    char buf[20];
    s.best_wpm = 0;
    memset(buf, 0, sizeof(buf));
    if (!FileOpen(1)) {
        return;
    }
    (void)FileReadBuffer(buf, (uint16_t)(sizeof(buf) - 1));
    FileClose();
    if (buf[0] == 'B' && buf[1] == '=') {
        s.best_wpm = (uint16_t)stock_parse_u32(&buf[2]);
    }
}

static void save_best(void)
{
    char buf[20];
    char n[8];
    memset(buf, 0, sizeof(buf));
    stock_u32_to_str(s.best_wpm, n, sizeof(n));
    buf[0] = 'B';
    buf[1] = '=';
    buf[2] = '\0';
    strncat(buf, n, sizeof(buf) - strlen(buf) - 1);
    if (FileOpen(1)) {
        FileWriteBuffer(buf, (uint16_t)sizeof(buf));
        FileClose();
    }
}

static void update_live_stats(void)
{
    uint32_t now = GetUptimeMilliseconds();
    uint32_t ms = (now >= s.start_ms) ? (now - s.start_ms) : 1u;
    uint32_t secs = stock_udiv32(ms + 999u, 1000u);
    uint32_t total;
    uint32_t acc;

    if (secs == 0) {
        secs = 1;
    }
    s.wpm = (uint16_t)stock_udiv32((uint32_t)s.hits * 12u, secs);
    total = (uint32_t)s.hits + (uint32_t)s.errors;
    if (total == 0) {
        total = 1;
    }
    acc = stock_udiv32((uint32_t)s.hits * 100u, total);
    s.accuracy = (acc > 100) ? 100 : (uint8_t)acc;
}

static void start_ex(void)
{
    const char *ex = cur_ex();
    s.pos = 0;
    s.errors = 0;
    s.hits = 0;
    s.wpm = 0;
    s.accuracy = 100;
    s.new_best = 0;
    s.flash_wrong = 0;
    s.start_ms = GetUptimeMilliseconds();
    memset(s.typed, 0, sizeof(s.typed));
    finger_hint(ex[0]);
    s.mode = MODE_RUN;
}

static void finish_ex(void)
{
    update_live_stats();
    s.new_best = 0;
    if (s.wpm > s.best_wpm && s.accuracy >= 90) {
        s.best_wpm = s.wpm;
        s.new_best = 1;
        save_best();
    }
    s.mode = MODE_DONE;
}

static void draw_guide_row(void)
{
    PutStringCentered(3, "a s d f    j k l ;");
    PutStringCentered(4, "L---------  --------R");
}

static void draw_menu(void)
{
    char line[40];
    char n[6];

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();
    PutStringCentered(1, "Touch Type");
    draw_guide_row();

    strncpy(line, "Lesson ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, k_lesson[s.lesson], sizeof(line) - strlen(line) - 1);
    PutStringCentered(6, line);
    PutStringCentered(7, k_guide[s.lesson]);

    if (s.best_wpm) {
        stock_u32_to_str(s.best_wpm, n, sizeof(n));
        strncpy(line, "Best ", sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, n, sizeof(line) - strlen(line) - 1);
        strncat(line, " WPM  Tab=lesson", sizeof(line) - strlen(line) - 1);
        PutStringCentered(8, line);
    } else {
        PutStringCentered(8, "Enter=start  Tab=lesson");
    }
}

static void draw_run(void)
{
    const char *ex = cur_ex();
    char line[40];
    char a[6], b[6], c[6];
    char marker[EX_LEN];
    uint8_t i;
    size_t len = strlen(ex);

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();

    stock_u32_to_str((uint32_t)(s.ex + 1), a, sizeof(a));
    stock_u32_to_str(EX_PER, b, sizeof(b));
    strncpy(line, k_lesson[s.lesson], sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, "  ", sizeof(line) - strlen(line) - 1);
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    strncat(line, "/", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    PutStringCentered(1, line);

    PutStringCentered(2, ex);

    /* Progress markers under the target: = done, > next, . todo */
    for (i = 0; i < (uint8_t)len && i < EX_LEN - 1; i++) {
        if (i < s.pos) {
            marker[i] = '=';
        } else if (i == s.pos) {
            marker[i] = '^';
        } else {
            marker[i] = '.';
        }
    }
    marker[i] = '\0';
    PutStringCentered(3, marker);

    ClearRowCols(4, 1, 40);
    SetCursor(4, 1, CURSOR_MODE_SHOW);
    PutStringRaw(s.typed);

    update_live_stats();
    stock_u32_to_str(s.hits, a, sizeof(a));
    stock_u32_to_str(s.errors, b, sizeof(b));
    stock_u32_to_str(s.wpm, c, sizeof(c));
    strncpy(line, a, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, " ok  ", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, " miss  ", sizeof(line) - strlen(line) - 1);
    strncat(line, c, sizeof(line) - strlen(line) - 1);
    strncat(line, " wpm", sizeof(line) - strlen(line) - 1);
    PutStringCentered(6, line);

    if (s.flash_wrong) {
        PutStringCentered(7, "Wrong key — try again");
        s.flash_wrong = 0;
    } else {
        strncpy(line, "next: ", sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, s.hint, sizeof(line) - strlen(line) - 1);
        PutStringCentered(7, line);
    }
    PutStringCentered(8, "Esc=menu  Find=skip");
}

static void draw_done(void)
{
    char line[40];
    char a[6], b[6];

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();
    PutStringCentered(1, s.new_best ? "New best!" : "Exercise done");

    stock_u32_to_str(s.wpm, a, sizeof(a));
    stock_u32_to_str(s.accuracy, b, sizeof(b));
    strncpy(line, a, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, " WPM   ", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, "% accurate", sizeof(line) - strlen(line) - 1);
    PutStringCentered(3, line);

    stock_u32_to_str(s.errors, a, sizeof(a));
    strncpy(line, "Misses: ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    PutStringCentered(5, line);

    if (s.ex + 1 < EX_PER) {
        PutStringCentered(8, "Enter=next  Esc=menu");
    } else if (s.lesson + 1 < LESSON_COUNT) {
        PutStringCentered(8, "Enter=next lesson  Esc");
    } else {
        PutStringCentered(8, "Course done! Enter=menu");
    }
}

static void draw(void)
{
    if (s.mode == MODE_RUN) {
        draw_run();
    } else if (s.mode == MODE_DONE) {
        draw_done();
    } else {
        draw_menu();
    }
}

static void advance_after_done(void)
{
    if (s.ex + 1 < EX_PER) {
        s.ex++;
        start_ex();
    } else if (s.lesson + 1 < LESSON_COUNT) {
        s.lesson++;
        s.ex = 0;
        start_ex();
    } else {
        s.mode = MODE_MENU;
        s.ex = 0;
    }
    draw();
}

static void on_char(char ch)
{
    const char *ex;
    size_t len;

    if (s.mode != MODE_RUN) {
        return;
    }
    if (ch < 0x20 || ch > 0x7e) {
        return;
    }
    ex = cur_ex();
    len = strlen(ex);
    if (s.pos >= len || s.pos >= EX_LEN - 1) {
        return;
    }

    if (ch == ex[s.pos]) {
        s.typed[s.pos] = ch;
        s.pos++;
        s.typed[s.pos] = '\0';
        if (s.hits < 9999) {
            s.hits++;
        }
        if (s.pos >= len) {
            finish_ex();
            draw();
        } else {
            finger_hint(ex[s.pos]);
            draw_run();
        }
    } else {
        if (s.errors < 9999) {
            s.errors++;
        }
        s.flash_wrong = 1;
        /* Strict coach: do not advance on mistakes. */
        draw_run();
    }
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.mode = MODE_MENU;
        break;
    case MSG_SETFOCUS:
        load_best();
        if (s.mode == MODE_RUN) {
            s.mode = MODE_MENU;
        }
        draw();
        break;
    case MSG_KILLFOCUS:
        break;
    case MSG_CHAR:
        on_char((char)(param & 0xFF));
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);

        if (code == KEY_ENTER || code == KEY_SEND) {
            if (s.mode == MODE_MENU) {
                s.ex = 0;
                start_ex();
                draw();
            } else if (s.mode == MODE_DONE) {
                advance_after_done();
            }
            break;
        }
        if (code == KEY_TAB || code == KEY_RIGHT) {
            if (s.mode == MODE_MENU) {
                s.lesson++;
                if (s.lesson >= LESSON_COUNT) {
                    s.lesson = 0;
                }
                draw();
            }
            break;
        }
        if (code == KEY_LEFT) {
            if (s.mode == MODE_MENU) {
                if (s.lesson == 0) {
                    s.lesson = LESSON_COUNT - 1;
                } else {
                    s.lesson--;
                }
                draw();
            }
            break;
        }
        if (code == KEY_ESC) {
            s.mode = MODE_MENU;
            draw();
            break;
        }
        if (code == KEY_FIND) {
            if (s.mode == MODE_RUN) {
                if (s.ex + 1 < EX_PER) {
                    s.ex++;
                    start_ex();
                } else {
                    s.mode = MODE_MENU;
                }
                draw();
            } else if (s.mode == MODE_DONE) {
                advance_after_done();
            }
            break;
        }
        if (code == KEY_CLEAR_FILE) {
            s.best_wpm = 0;
            save_best();
            s.mode = MODE_MENU;
            draw();
            break;
        }
        if (s.mode == MODE_RUN && (code == KEY_BACKSPACE || code == KEY_DELETE)) {
            /* Learning mode: backspace undoes last correct char. */
            if (s.pos > 0) {
                s.pos--;
                s.typed[s.pos] = '\0';
                if (s.hits > 0) {
                    s.hits--;
                }
                finger_hint(cur_ex()[s.pos]);
                draw_run();
            }
            break;
        }
        break;
    }
    default:
        break;
    }
}
