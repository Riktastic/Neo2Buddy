/**
 * @file WordTree.c
 * @brief On-device AlphaWord word-count goal tree (no buddy required).
 *
 * FileSetFolder → AlphaWord files 1..8. Own file stores G=<goal>.
 * Enter=set goal  Find=refresh counts  Clear File=reset goal
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B4
#define ALPHAWORD_ID 0xA000
#define DEFAULT_GOAL 10000u
#define AW_FILES 8
#define READ_CHUNK 64
#define MAX_CHUNKS_PER_FILE 2048u

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Word Tree")
    APPLET_INFO("AlphaWord writing stats tree")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 64,
APPLET_HEADER_END

typedef struct {
    uint32_t words;
    uint32_t chars;
    uint32_t files;
    uint32_t goal;
    uint8_t have_aw;
    uint8_t dirty;
} tree_state_t;

static tree_state_t s;

static int is_word_char(unsigned char c)
{
    if (c == 0 || c == 0xa4 || c == 0xa7) {
        return 0;
    }
    if (c <= 0x20) {
        return 0;
    }
    return 1;
}

static void save_goal(void)
{
    char buf[24];
    char g[12];
    memset(buf, 0, sizeof(buf));
    stock_u32_to_str(s.goal ? s.goal : DEFAULT_GOAL, g, sizeof(g));
    strncpy(buf, "G=", sizeof(buf) - 1);
    strncat(buf, g, sizeof(buf) - strlen(buf) - 1);
    strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
    if (FileOpen(1)) {
        FileWriteBuffer(buf, (uint16_t)sizeof(buf));
        FileClose();
        s.dirty = 0;
    }
}

static void load_goal(void)
{
    char buf[24];
    char *g;
    s.goal = DEFAULT_GOAL;
    s.dirty = 0;
    memset(buf, 0, sizeof(buf));
    if (!FileOpen(1)) {
        return;
    }
    (void)FileReadBuffer(buf, (uint16_t)(sizeof(buf) - 1));
    FileClose();
    g = strstr(buf, "G=");
    if (g) {
        s.goal = stock_parse_u32(g + 2);
        if (s.goal == 0) {
            s.goal = DEFAULT_GOAL;
        }
    }
}

static void count_open_file(uint32_t *words, uint32_t *chars, uint8_t *had_text)
{
    char buf[READ_CHUNK];
    uint32_t chunks = 0;
    uint8_t in_word = 0;
    uint8_t pad_streak = 0;

    *had_text = 0;
    for (;;) {
        int n;
        int i;
        uint8_t any_content = 0;
        if (chunks >= MAX_CHUNKS_PER_FILE) {
            break;
        }
        chunks++;
        memset(buf, 0, sizeof(buf));
        n = FileReadBuffer(buf, (uint16_t)READ_CHUNK);
        if (n <= 0) {
            break;
        }
        if (n > READ_CHUNK) {
            n = READ_CHUNK;
        }
        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (is_word_char(c)) {
                (*chars)++;
                *had_text = 1;
                any_content = 1;
                pad_streak = 0;
                if (!in_word) {
                    (*words)++;
                    in_word = 1;
                }
            } else {
                in_word = 0;
                if (c == 0 || c == 0xa4 || c == 0xa7) {
                    pad_streak++;
                } else {
                    pad_streak = 0;
                }
            }
        }
        /* Trailing AlphaWord padding — stop early. */
        if (!any_content && pad_streak >= (uint8_t)n) {
            break;
        }
        if (n < READ_CHUNK) {
            break;
        }
    }
}

static void scan_alphaword(void)
{
    uint8_t aw;
    uint8_t self;
    uint8_t fi;
    uint32_t words = 0;
    uint32_t chars = 0;
    uint32_t files = 0;

    s.have_aw = 0;
    s.words = 0;
    s.chars = 0;
    s.files = 0;

    aw = AppletFindById(ALPHAWORD_ID);
    if (aw == 0) {
        return;
    }
    self = AppletFindById(APP_ID);

    ClearRowCols(4, 1, 40);
    PutStringCentered(4, "Counting AlphaWord…");
    (void)FileSetFolder(aw);

    for (fi = 1; fi <= AW_FILES; fi++) {
        uint8_t had = 0;
        uint32_t w0 = words;
        if (!FileOpen(fi)) {
            continue;
        }
        count_open_file(&words, &chars, &had);
        FileClose();
        if (had && words > w0) {
            files++;
        }
        ProgressBar(5, fi, AW_FILES);
    }

    if (self != 0) {
        (void)FileSetFolder(self);
    }

    s.words = words;
    s.chars = chars;
    s.files = files;
    s.have_aw = 1;
}

static uint8_t progress_pct(void)
{
    uint32_t p;
    if (s.goal == 0) {
        return 0;
    }
    if (s.words >= s.goal) {
        return 100;
    }
    p = stock_udiv32(s.words * 100u, s.goal);
    return (p > 100) ? 100 : (uint8_t)p;
}

static void draw_tree(uint8_t pct)
{
    ClearRowCols(4, 1, 40);
    ClearRowCols(5, 1, 40);
    ClearRowCols(6, 1, 40);
    ClearRowCols(7, 1, 40);
    if (pct < 10) {
        PutStringCentered(6, ".");
        PutStringCentered(7, "|");
    } else if (pct < 25) {
        PutStringCentered(5, "/\\");
        PutStringCentered(6, "||");
        PutStringCentered(7, "==");
    } else if (pct < 45) {
        PutStringCentered(4, " /\\");
        PutStringCentered(5, "/\\/\\");
        PutStringCentered(6, " ||");
        PutStringCentered(7, "====");
    } else if (pct < 70) {
        PutStringCentered(4, "  /\\");
        PutStringCentered(5, " /\\/\\");
        PutStringCentered(6, "/\\/\\/\\");
        PutStringCentered(7, "======");
    } else if (pct < 100) {
        PutStringCentered(4, "   /\\");
        PutStringCentered(5, "  /\\/\\");
        PutStringCentered(6, " /\\/\\/\\");
        PutStringCentered(7, "/\\/\\/\\/\\");
    } else {
        PutStringCentered(4, "  * /\\ *");
        PutStringCentered(5, "  /\\/\\/\\");
        PutStringCentered(6, " /\\/\\/\\/\\");
        PutStringCentered(7, "===||||===");
    }
}

static void draw_screen(void)
{
    char line[41];
    char a[12], b[12], p[4];
    uint8_t pct = progress_pct();

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearRowCols(1, 1, 40);
    PutStringCentered(1, "Word Tree");
    ClearRowCols(2, 1, 40);
    if (!s.have_aw) {
        PutStringCentered(2, "AlphaWord not found");
    } else {
        stock_u32_to_str(s.words, a, sizeof(a));
        stock_u32_to_str(s.goal, b, sizeof(b));
        stock_u32_to_str(pct, p, sizeof(p));
        strncpy(line, a, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, "/", sizeof(line) - strlen(line) - 1);
        strncat(line, b, sizeof(line) - strlen(line) - 1);
        strncat(line, " (", sizeof(line) - strlen(line) - 1);
        strncat(line, p, sizeof(line) - strlen(line) - 1);
        strncat(line, "%)", sizeof(line) - strlen(line) - 1);
        PutStringCentered(2, line);
    }
    ClearRowCols(3, 1, 40);
    if (s.have_aw) {
        stock_u32_to_str(s.files, a, sizeof(a));
        stock_u32_to_str(s.chars, b, sizeof(b));
        strncpy(line, a, sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, " files  ", sizeof(line) - strlen(line) - 1);
        strncat(line, b, sizeof(line) - strlen(line) - 1);
        strncat(line, " chars", sizeof(line) - strlen(line) - 1);
        PutStringCentered(3, line);
    } else {
        PutStringCentered(3, "Need AlphaWord installed");
    }
    draw_tree(s.have_aw ? pct : 0);
    ClearRowCols(8, 1, 40);
    PutStringCentered(8, "Enter=goal  Find=refresh");
}

static void refresh_all(void)
{
    ClearScreen();
    PutStringCentered(1, "Word Tree");
    load_goal();
    scan_alphaword();
    draw_screen();
}

static int prompt_number(const char *label, uint32_t *out_val)
{
    static const Key_e exit_keys[] = { KEY_ESC, KEY_ENTER, KEY_NONE };
    char buf[12];
    uint8_t len = 0;
    KeyMod_e key;

    buf[0] = '\0';
    ClearRowCols(8, 1, 40);
    SetCursor(8, 1, CURSOR_MODE_SHOW);
    PutStringRaw(label);
    key = (KeyMod_e)TextBox(buf, &len, 11, exit_keys, false);
    SetCursorMode(CURSOR_MODE_HIDE);
    if ((key & 0xFF) == KEY_ESC || len == 0) {
        return 0;
    }
    *out_val = stock_parse_u32(buf);
    return 1;
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        s.goal = DEFAULT_GOAL;
        s.have_aw = 0;
        s.dirty = 0;
        break;
    case MSG_SETFOCUS:
        refresh_all();
        break;
    case MSG_KILLFOCUS:
        if (s.dirty) {
            save_goal();
        }
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_ENTER || code == KEY_SEND) {
            uint32_t g = s.goal;
            if (prompt_number("Week goal: ", &g) && g > 0) {
                s.goal = g;
                s.dirty = 1;
                save_goal();
            }
            draw_screen();
        } else if (code == KEY_FIND || code == KEY_HOME || code == KEY_SPELL_CHECK) {
            refresh_all();
        } else if (code == KEY_CLEAR_FILE) {
            s.goal = DEFAULT_GOAL;
            s.dirty = 1;
            save_goal();
            draw_screen();
        }
        break;
    }
    default:
        break;
    }
}
