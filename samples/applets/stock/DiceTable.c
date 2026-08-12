/**
 * @file DiceTable.c
 * @brief D&D dice + session notes. Split screen: notes | dice.
 *
 * File keys F1–F7 roll d4/d6/d8/d10/d12/d20/d100; F8 clears last line.
 * Space=2d6  Enter=note  Up/Down=scroll  Find=save  Clear File=wipe
 * Natural 20 / nat-1 flash the LCD. Session auto-saves on leave / Find.
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B2
#define NOTE_COLS 21
#define DICE_COL 23
#define NOTE_ROWS 5
#define LOG_MAX 14
#define NOTE_LINE 32

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Dice Table")
    APPLET_INFO("D&D dice + session notes")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 512,
APPLET_HEADER_END

typedef struct {
    char notes[LOG_MAX][NOTE_LINE];
    uint8_t note_count;
    uint8_t note_scroll;
    char status[18];
    uint16_t rng;
    uint8_t dirty;
} dice_state_t;

static dice_state_t s;
static char s_io[420];

static uint16_t rng_next(void)
{
    s.rng ^= (uint16_t)(GetUptimeMilliseconds() & 0xFFFF);
    s.rng = (uint16_t)(s.rng * 1103515245u + 12345u);
    return s.rng;
}

static uint8_t roll_die(uint8_t sides)
{
    uint16_t r;
    uint16_t limit = 0;
    uint16_t acc = 0;
    if (sides < 2) {
        return 1;
    }
    while ((uint16_t)(acc + sides) > acc && (uint16_t)(acc + sides) <= 65535u) {
        acc = (uint16_t)(acc + sides);
    }
    limit = acc;
    if (limit < sides) {
        limit = sides;
    }
    do {
        r = rng_next();
    } while (r >= limit);
    return (uint8_t)(stock_urem16(r, sides) + 1);
}

static void set_status(const char *msg)
{
    strncpy(s.status, msg ? msg : "", sizeof(s.status) - 1);
    s.status[sizeof(s.status) - 1] = '\0';
}

static void append_note(const char *line)
{
    if (!line || !line[0]) {
        return;
    }
    if (s.note_count >= LOG_MAX) {
        memmove(s.notes[0], s.notes[1], (LOG_MAX - 1) * NOTE_LINE);
        s.note_count = LOG_MAX - 1;
    }
    strncpy(s.notes[s.note_count], line, NOTE_LINE - 1);
    s.notes[s.note_count][NOTE_LINE - 1] = '\0';
    s.note_count++;
    s.dirty = 1;
    if (s.note_count > NOTE_ROWS) {
        s.note_scroll = (uint8_t)(s.note_count - NOTE_ROWS);
    }
}

static void save_session(void)
{
    uint8_t i;
    size_t off = 0;
    memset(s_io, 0, sizeof(s_io));
    for (i = 0; i < s.note_count && off + NOTE_LINE < sizeof(s_io); i++) {
        strncpy(&s_io[off], s.notes[i], NOTE_LINE - 1);
        off += strlen(&s_io[off]);
        s_io[off++] = '\n';
    }
    if (FileOpen(1)) {
        FileWriteBuffer(s_io, (uint16_t)sizeof(s_io));
        FileClose();
        s.dirty = 0;
    }
}

static void load_session(void)
{
    char *p;
    char *line;
    memset(s.notes, 0, sizeof(s.notes));
    s.note_count = 0;
    s.note_scroll = 0;
    s.dirty = 0;
    memset(s_io, 0, sizeof(s_io));
    if (!FileOpen(1)) {
        return;
    }
    (void)FileReadBuffer(s_io, (uint16_t)(sizeof(s_io) - 1));
    FileClose();
    p = s_io;
    while (*p && s.note_count < LOG_MAX) {
        line = p;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p++ = '\0';
        }
        if (line[0]) {
            strncpy(s.notes[s.note_count], line, NOTE_LINE - 1);
            s.notes[s.note_count][NOTE_LINE - 1] = '\0';
            s.note_count++;
        }
    }
    if (s.note_count > NOTE_ROWS) {
        s.note_scroll = (uint8_t)(s.note_count - NOTE_ROWS);
    }
}

static void format_roll(uint8_t sides, uint8_t val, char *out, size_t out_sz)
{
    char sb[4], vb[4];
    if (sides == 20 && val == 20) {
        strncpy(out, "d20 -> 20 NAT!", out_sz - 1);
    } else if (sides == 20 && val == 1) {
        strncpy(out, "d20 -> 1 FAIL", out_sz - 1);
    } else {
        stock_u32_to_str(sides, sb, sizeof(sb));
        stock_u32_to_str(val, vb, sizeof(vb));
        strncpy(out, "d", out_sz - 1);
        out[out_sz - 1] = '\0';
        strncat(out, sb, out_sz - strlen(out) - 1);
        strncat(out, " -> ", out_sz - strlen(out) - 1);
        strncat(out, vb, out_sz - strlen(out) - 1);
    }
    out[out_sz - 1] = '\0';
}

static void flash_lcd(uint8_t times, uint8_t on_cs, uint8_t off_cs)
{
    uint8_t i;
    for (i = 0; i < times; i++) {
        LCD_CMD_REG_LEFT = LCD_CMD_REG_RIGHT = LCD_CMD_REVERSE(1);
        SleepCentiseconds(on_cs);
        LCD_CMD_REG_LEFT = LCD_CMD_REG_RIGHT = LCD_CMD_REVERSE(0);
        SleepCentiseconds(off_cs);
    }
}

static void do_roll(uint8_t sides)
{
    char line[NOTE_LINE];
    uint8_t val = roll_die(sides);
    format_roll(sides, val, line, sizeof(line));
    append_note(line);
    set_status(line);
    if (sides == 20 && val == 20) {
        flash_lcd(3, 12, 8);
    } else if (sides == 20 && val == 1) {
        flash_lcd(1, 20, 10);
    }
}

static void do_roll_2d6(void)
{
    char line[NOTE_LINE];
    char a[4], b[4], t[4];
    uint8_t d1 = roll_die(6);
    uint8_t d2 = roll_die(6);
    uint8_t sum = (uint8_t)(d1 + d2);
    stock_u32_to_str(d1, a, sizeof(a));
    stock_u32_to_str(d2, b, sizeof(b));
    stock_u32_to_str(sum, t, sizeof(t));
    strncpy(line, "2d6 ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    strncat(line, "+", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, "=", sizeof(line) - strlen(line) - 1);
    strncat(line, t, sizeof(line) - strlen(line) - 1);
    append_note(line);
    set_status(line);
    if (sum == 12) {
        flash_lcd(2, 10, 8);
    }
}

static void draw_notes(void)
{
    uint8_t r;
    for (r = 0; r < NOTE_ROWS; r++) {
        uint8_t row = (uint8_t)(2 + r);
        ClearRowCols(row, 1, NOTE_COLS);
        if ((uint8_t)(s.note_scroll + r) < s.note_count) {
            char tmp[NOTE_COLS + 1];
            strncpy(tmp, s.notes[s.note_scroll + r], NOTE_COLS);
            tmp[NOTE_COLS] = '\0';
            SetCursor(row, 1, CURSOR_MODE_HIDE);
            PutString(tmp);
        } else if (s.note_count == 0 && r == 0) {
            SetCursor(row, 1, CURSOR_MODE_HIDE);
            PutString("(rolls & notes)");
        }
    }
}

static void draw_dice_panel(void)
{
    uint8_t row;
    for (row = 2; row <= 6; row++) {
        ClearRowCols(row, DICE_COL, 40);
        SetCursor(row, 22, CURSOR_MODE_HIDE);
        PutChar('|');
    }
    SetCursor(2, DICE_COL, CURSOR_MODE_HIDE);
    PutString("F1-7 dice");
    SetCursor(3, DICE_COL, CURSOR_MODE_HIDE);
    PutString("4 6 8 10 12");
    SetCursor(4, DICE_COL, CURSOR_MODE_HIDE);
    PutString("20 100");
    SetCursor(5, DICE_COL, CURSOR_MODE_HIDE);
    PutString("Spc=2d6");
    SetCursor(6, DICE_COL, CURSOR_MODE_HIDE);
    PutString(s.status[0] ? s.status : "F8=undo");
}

static void draw_screen(void)
{
    SetCursorMode(CURSOR_MODE_HIDE);
    ClearRowCols(1, 1, 40);
    PutStringCentered(1, "Dice Table");
    draw_notes();
    draw_dice_panel();
    ClearRowCols(8, 1, 40);
    PutStringCentered(8, s.dirty ? "Note Find=save* F8=undo" : "Note Find=save F8=undo");
}

static void add_note_prompt(void)
{
    static const Key_e exit_keys[] = { KEY_ESC, KEY_ENTER, KEY_NONE };
    char buf[NOTE_COLS + 1];
    uint8_t len = 0;
    KeyMod_e key;

    buf[0] = '\0';
    ClearRowCols(8, 1, 40);
    SetCursor(8, 1, CURSOR_MODE_SHOW);
    PutStringRaw("Note: ");
    key = (KeyMod_e)TextBox(buf, &len, NOTE_COLS, exit_keys, false);
    SetCursorMode(CURSOR_MODE_HIDE);
    if ((key & 0xFF) != KEY_ESC && len > 0) {
        append_note(buf);
        set_status("note+");
    }
    draw_screen();
}

static void clear_last(void)
{
    if (s.note_count == 0) {
        set_status("empty");
        draw_screen();
        return;
    }
    s.note_count--;
    s.notes[s.note_count][0] = '\0';
    s.dirty = 1;
    if (s.note_scroll > 0 && s.note_scroll >= s.note_count) {
        s.note_scroll = s.note_count > NOTE_ROWS ? (uint8_t)(s.note_count - NOTE_ROWS) : 0;
    }
    set_status("undone");
    draw_screen();
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.rng = (uint16_t)GetUptimeMilliseconds();
        load_session();
        break;
    case MSG_SETFOCUS:
        draw_screen();
        break;
    case MSG_KILLFOCUS:
        if (s.dirty) {
            save_session();
        }
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_FILE_1) {
            do_roll(4);
            draw_screen();
        } else if (code == KEY_FILE_2) {
            do_roll(6);
            draw_screen();
        } else if (code == KEY_FILE_3) {
            do_roll(8);
            draw_screen();
        } else if (code == KEY_FILE_4) {
            do_roll(10);
            draw_screen();
        } else if (code == KEY_FILE_5) {
            do_roll(12);
            draw_screen();
        } else if (code == KEY_FILE_6) {
            do_roll(20);
            draw_screen();
        } else if (code == KEY_FILE_7) {
            do_roll(100);
            draw_screen();
        } else if (code == KEY_FILE_8) {
            clear_last();
        } else if (code == KEY_SPACE || code == KEY_SPELL_CHECK) {
            do_roll_2d6();
            draw_screen();
        } else if (code == KEY_ENTER || code == KEY_SEND) {
            add_note_prompt();
        } else if (code == KEY_UP) {
            if (s.note_scroll > 0) {
                s.note_scroll--;
                draw_notes();
            }
        } else if (code == KEY_DOWN) {
            if (s.note_count > NOTE_ROWS &&
                s.note_scroll < (uint8_t)(s.note_count - NOTE_ROWS)) {
                s.note_scroll++;
                draw_notes();
            }
        } else if (code == KEY_FIND) {
            s.note_scroll = 0;
            save_session();
            ClearRowCols(8, 1, 40);
            PutStringCentered(8, "Saved");
            SleepCentiseconds(40);
            draw_screen();
        } else if (code == KEY_CLEAR_FILE) {
            memset(s.notes, 0, sizeof(s.notes));
            s.note_count = 0;
            s.note_scroll = 0;
            s.dirty = 1;
            set_status("wiped");
            save_session();
            draw_screen();
        }
        break;
    }
    default:
        break;
    }
}
