/**
 * @file TaskPad.c
 * @brief Lightweight task list — add, toggle, remove. Persists in file 1.
 *
 * Enter=add  Space=toggle  Delete=remove  Up/Down=move
 * Find=save  Clear File=wipe all
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B1
#define MAX_TASKS 10
#define TASK_LEN 32
#define VIEW_ROWS 6

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Task Pad")
    APPLET_INFO("Simple task checklist")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 512,
APPLET_HEADER_END

typedef struct {
    char text[MAX_TASKS][TASK_LEN];
    uint8_t done[MAX_TASKS];
    uint8_t count;
    uint8_t sel;
    uint8_t scroll;
    uint8_t dirty;
} task_state_t;

static task_state_t s;
static char s_io[360];

static void save_tasks(void)
{
    uint8_t i;
    size_t off = 0;
    memset(s_io, 0, sizeof(s_io));
    for (i = 0; i < s.count && off + TASK_LEN + 2 < sizeof(s_io); i++) {
        s_io[off++] = s.done[i] ? 'x' : ' ';
        s_io[off++] = '|';
        strncpy(&s_io[off], s.text[i], TASK_LEN - 1);
        off += strlen(&s_io[off]);
        s_io[off++] = '\n';
    }
    if (FileOpen(1)) {
        FileWriteBuffer(s_io, (uint16_t)sizeof(s_io));
        FileClose();
        s.dirty = 0;
    }
}

static void load_tasks(void)
{
    char *p;
    char *line;
    memset(&s, 0, sizeof(s));
    memset(s_io, 0, sizeof(s_io));
    if (!FileOpen(1)) {
        return;
    }
    (void)FileReadBuffer(s_io, (uint16_t)(sizeof(s_io) - 1));
    FileClose();
    p = s_io;
    while (*p && s.count < MAX_TASKS) {
        line = p;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p++ = '\0';
        }
        if (line[0] && line[1] == '|') {
            s.done[s.count] = (line[0] == 'x' || line[0] == 'X') ? 1 : 0;
            strncpy(s.text[s.count], line + 2, TASK_LEN - 1);
            s.text[s.count][TASK_LEN - 1] = '\0';
            if (s.text[s.count][0]) {
                s.count++;
            }
        }
    }
}

static void ensure_sel_visible(void)
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
    char line[41];
    char n[4];
    uint8_t done = 0;

    for (r = 0; r < s.count; r++) {
        if (s.done[r]) {
            done++;
        }
    }

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearRowCols(1, 1, 40);
    if (s.count == 0) {
        PutStringCentered(1, "Task Pad");
    } else {
        stock_u32_to_str(done, n, sizeof(n));
        strncpy(line, "Tasks ", sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, n, sizeof(line) - strlen(line) - 1);
        strncat(line, "/", sizeof(line) - strlen(line) - 1);
        stock_u32_to_str(s.count, n, sizeof(n));
        strncat(line, n, sizeof(line) - strlen(line) - 1);
        PutStringCentered(1, line);
    }

    for (r = 0; r < VIEW_ROWS; r++) {
        uint8_t idx = (uint8_t)(s.scroll + r);
        uint8_t row = (uint8_t)(2 + r);
        ClearRowCols(row, 1, 40);
        if (idx >= s.count) {
            if (s.count == 0 && r == 1) {
                PutStringCentered(row, "Enter adds a task");
            }
            continue;
        }
        line[0] = (idx == s.sel) ? '>' : ' ';
        line[1] = '[';
        line[2] = s.done[idx] ? 'x' : ' ';
        line[3] = ']';
        line[4] = ' ';
        strncpy(line + 5, s.text[idx], 34);
        line[40] = '\0';
        SetCursor(row, 1, CURSOR_MODE_HIDE);
        PutString(line);
    }
    ClearRowCols(8, 1, 40);
    if (s.count && done == s.count) {
        PutStringCentered(8, "All done! Nice work");
    } else {
        PutStringCentered(8, s.dirty ? "Spc=tog Del Find=save*" : "Spc=tog Del Find=save");
    }
}

static void add_task(void)
{
    static const Key_e exit_keys[] = { KEY_ESC, KEY_ENTER, KEY_NONE };
    char buf[TASK_LEN];
    uint8_t len = 0;
    KeyMod_e key;

    if (s.count >= MAX_TASKS) {
        ClearRowCols(8, 1, 40);
        PutStringCentered(8, "List full (10 max)");
        SleepCentiseconds(60);
        draw_screen();
        return;
    }
    buf[0] = '\0';
    ClearRowCols(8, 1, 40);
    SetCursor(8, 1, CURSOR_MODE_SHOW);
    PutStringRaw("New: ");
    key = (KeyMod_e)TextBox(buf, &len, TASK_LEN - 1, exit_keys, false);
    SetCursorMode(CURSOR_MODE_HIDE);
    if ((key & 0xFF) != KEY_ESC && len > 0) {
        strncpy(s.text[s.count], buf, TASK_LEN - 1);
        s.text[s.count][TASK_LEN - 1] = '\0';
        s.done[s.count] = 0;
        s.sel = s.count;
        s.count++;
        s.dirty = 1;
        ensure_sel_visible();
    }
    draw_screen();
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        load_tasks();
        break;
    case MSG_SETFOCUS:
        draw_screen();
        break;
    case MSG_KILLFOCUS:
        if (s.dirty) {
            save_tasks();
        }
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_ENTER || code == KEY_SEND) {
            add_task();
        } else if (code == KEY_SPACE || code == KEY_SPELL_CHECK) {
            if (s.count && s.sel < s.count) {
                s.done[s.sel] = s.done[s.sel] ? 0 : 1;
                s.dirty = 1;
                draw_screen();
            }
        } else if (code == KEY_DELETE || code == KEY_BACKSPACE) {
            if (s.count && s.sel < s.count) {
                uint8_t i;
                for (i = s.sel; i + 1 < s.count; i++) {
                    memcpy(s.text[i], s.text[i + 1], TASK_LEN);
                    s.done[i] = s.done[i + 1];
                }
                s.count--;
                if (s.sel >= s.count && s.count > 0) {
                    s.sel = (uint8_t)(s.count - 1);
                }
                s.dirty = 1;
                ensure_sel_visible();
                draw_screen();
            }
        } else if (code == KEY_UP) {
            if (s.sel > 0) {
                s.sel--;
                ensure_sel_visible();
                draw_screen();
            }
        } else if (code == KEY_DOWN) {
            if (s.count && s.sel + 1 < s.count) {
                s.sel++;
                ensure_sel_visible();
                draw_screen();
            }
        } else if (code == KEY_FIND) {
            save_tasks();
            ClearRowCols(8, 1, 40);
            PutStringCentered(8, "Saved");
            SleepCentiseconds(40);
            draw_screen();
        } else if (code == KEY_CLEAR_FILE) {
            memset(&s, 0, sizeof(s));
            s.dirty = 1;
            save_tasks();
            draw_screen();
        }
        break;
    }
    default:
        break;
    }
}
