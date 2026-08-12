/**
 * @file ScriptPad.c
 * @brief Dialogue / screenplay sketch pad.
 *
 * Enter=new line  Tab=insert speaker cue (then edit)  Find=save
 * Up/Down=select  Delete=remove  Clear File=wipe
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B3
#define MAX_LINES 10
#define LINE_LEN 36
#define VIEW_ROWS 6
#define MAX_SPEAKERS 6

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Script Pad")
    APPLET_INFO("Dialogue & screenplay notes")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 512,
APPLET_HEADER_END

static const char k_speakers[MAX_SPEAKERS][10] = {
    "ALICE", "BOB", "NARRATOR", "INT.", "EXT.", "CUT TO:"
};

static const char *speaker_name(uint8_t idx)
{
    if (idx >= MAX_SPEAKERS) {
        idx = 0;
    }
    return k_speakers[idx];
}

typedef struct {
    char lines[MAX_LINES][LINE_LEN];
    uint8_t count;
    uint8_t sel;
    uint8_t scroll;
    uint8_t speaker;
    uint8_t dirty;
} script_state_t;

static script_state_t s;
static char s_io[400];

static void save_script(void)
{
    uint8_t i;
    size_t off = 0;
    memset(s_io, 0, sizeof(s_io));
    for (i = 0; i < s.count && off + LINE_LEN < sizeof(s_io); i++) {
        strncpy(&s_io[off], s.lines[i], LINE_LEN - 1);
        off += strlen(&s_io[off]);
        s_io[off++] = '\n';
    }
    if (FileOpen(1)) {
        FileWriteBuffer(s_io, (uint16_t)sizeof(s_io));
        FileClose();
        s.dirty = 0;
    }
}

static void load_script(void)
{
    char *p;
    char *line;
    memset(&s, 0, sizeof(s));
    memset(s_io, 0, sizeof(s_io));
    if (!FileOpen(1)) {
        strncpy(s.lines[0], "FADE IN:", LINE_LEN - 1);
        strncpy(s.lines[1], "INT. LOCATION - DAY", LINE_LEN - 1);
        s.count = 2;
        return;
    }
    (void)FileReadBuffer(s_io, (uint16_t)(sizeof(s_io) - 1));
    FileClose();
    p = s_io;
    while (*p && s.count < MAX_LINES) {
        line = p;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p++ = '\0';
        }
        if (line[0]) {
            strncpy(s.lines[s.count], line, LINE_LEN - 1);
            s.lines[s.count][LINE_LEN - 1] = '\0';
            s.count++;
        }
    }
    if (s.count == 0) {
        strncpy(s.lines[0], "FADE IN:", LINE_LEN - 1);
        s.count = 1;
    }
}

static void ensure_visible(void)
{
    if (s.sel < s.scroll) {
        s.scroll = s.sel;
    }
    if (s.sel >= (uint8_t)(s.scroll + VIEW_ROWS)) {
        s.scroll = (uint8_t)(s.sel - VIEW_ROWS + 1);
    }
}

static void draw_screen(void)
{
    uint8_t r;
    char rowbuf[41];
    char hint[41];
    char n[4];

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearRowCols(1, 1, 40);
    stock_u32_to_str(s.count, n, sizeof(n));
    strncpy(rowbuf, "Script ", sizeof(rowbuf) - 1);
    rowbuf[sizeof(rowbuf) - 1] = '\0';
    strncat(rowbuf, n, sizeof(rowbuf) - strlen(rowbuf) - 1);
    strncat(rowbuf, "/", sizeof(rowbuf) - strlen(rowbuf) - 1);
    stock_u32_to_str(MAX_LINES, n, sizeof(n));
    strncat(rowbuf, n, sizeof(rowbuf) - strlen(rowbuf) - 1);
    PutStringCentered(1, rowbuf);

    for (r = 0; r < VIEW_ROWS; r++) {
        uint8_t idx = (uint8_t)(s.scroll + r);
        uint8_t row = (uint8_t)(2 + r);
        ClearRowCols(row, 1, 40);
        if (idx >= s.count) {
            continue;
        }
        rowbuf[0] = (idx == s.sel) ? '>' : ' ';
        strncpy(rowbuf + 1, s.lines[idx], 38);
        rowbuf[40] = '\0';
        SetCursor(row, 1, CURSOR_MODE_HIDE);
        PutString(rowbuf);
    }
    ClearRowCols(8, 1, 40);
    strncpy(hint, "Tab=", sizeof(hint) - 1);
    hint[sizeof(hint) - 1] = '\0';
    strncat(hint, speaker_name(s.speaker), sizeof(hint) - strlen(hint) - 1);
    strncat(hint, s.dirty ? " Find*" : " Find", sizeof(hint) - strlen(hint) - 1);
    PutStringCentered(8, hint);
}

static void add_line(int as_speaker)
{
    static const Key_e exit_keys[] = { KEY_ESC, KEY_ENTER, KEY_NONE };
    char buf[LINE_LEN];
    uint8_t len = 0;
    KeyMod_e key;

    if (s.count >= MAX_LINES) {
        ClearRowCols(8, 1, 40);
        PutStringCentered(8, "Script full (10)");
        SleepCentiseconds(50);
        draw_screen();
        return;
    }
    buf[0] = '\0';
    if (as_speaker) {
        strncpy(buf, speaker_name(s.speaker), LINE_LEN - 1);
        strncat(buf, ":", sizeof(buf) - strlen(buf) - 1);
        len = (uint8_t)strlen(buf);
        s.speaker = (uint8_t)(s.speaker + 1);
        if (s.speaker >= MAX_SPEAKERS) {
            s.speaker = 0;
        }
    }
    ClearRowCols(8, 1, 40);
    SetCursor(8, 1, CURSOR_MODE_SHOW);
    key = (KeyMod_e)TextBox(buf, &len, LINE_LEN - 1, exit_keys, false);
    SetCursorMode(CURSOR_MODE_HIDE);
    if ((key & 0xFF) != KEY_ESC && len > 0) {
        strncpy(s.lines[s.count], buf, LINE_LEN - 1);
        s.lines[s.count][LINE_LEN - 1] = '\0';
        s.sel = s.count;
        s.count++;
        s.dirty = 1;
        ensure_visible();
    }
    draw_screen();
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        load_script();
        break;
    case MSG_SETFOCUS:
        draw_screen();
        break;
    case MSG_KILLFOCUS:
        if (s.dirty) {
            save_script();
        }
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_ENTER || code == KEY_SEND) {
            add_line(0);
        } else if (code == KEY_TAB) {
            add_line(1);
        } else if (code == KEY_DELETE || code == KEY_BACKSPACE) {
            if (s.count && s.sel < s.count) {
                uint8_t i;
                for (i = s.sel; i + 1 < s.count; i++) {
                    memcpy(s.lines[i], s.lines[i + 1], LINE_LEN);
                }
                s.count--;
                if (s.sel >= s.count && s.count) {
                    s.sel = (uint8_t)(s.count - 1);
                }
                s.dirty = 1;
                ensure_visible();
                draw_screen();
            }
        } else if (code == KEY_UP) {
            if (s.sel > 0) {
                s.sel--;
                ensure_visible();
                draw_screen();
            }
        } else if (code == KEY_DOWN) {
            if (s.count && s.sel + 1 < s.count) {
                s.sel++;
                ensure_visible();
                draw_screen();
            }
        } else if (code == KEY_FIND) {
            save_script();
            ClearRowCols(8, 1, 40);
            PutStringCentered(8, "Saved");
            SleepCentiseconds(40);
            draw_screen();
        } else if (code == KEY_CLEAR_FILE) {
            memset(&s, 0, sizeof(s));
            strncpy(s.lines[0], "FADE IN:", LINE_LEN - 1);
            s.count = 1;
            s.dirty = 1;
            save_script();
            draw_screen();
        }
        break;
    }
    default:
        break;
    }
}
