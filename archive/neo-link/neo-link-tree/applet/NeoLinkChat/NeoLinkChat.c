/**
 * @file NeoLinkChat.c
 * @brief Neo Link Chat SmartApplet — full UI + NeoLinkIn/Out mailboxes.
 *
 * Build: tools/build-neolinkchat-betawise.ps1 (BetaWise v0.2).
 * Rules: neo-link docs/requirements.md, docs/os3k-applet.md.
 */

#include "os3k.h"
#include "neo_link_limits.h"
#include "neo_link_inbox.h"
#include "neo_link_os3k.h"
#include "neo_link_applet_guard.h"

#include <string.h>

bool neo_link_applet_send_chat(const char *prompt, bool try_hid);


#define COLS NEO_LCD_COL_LAST
/* Enough wrapped lines for REPLY_MAX≈200 @ 40 cols, with scroll headroom. */
#define MAX_LINES 8

#define COL_FIRST NEO_LCD_COL_FIRST
#define COL_LAST NEO_LCD_COL_LAST
#define ROW_TITLE NEO_LCD_ROW_TITLE
#define ROW_USB NEO_LCD_ROW_USB
#define VIEW_TOP NEO_LCD_ROW_VIEW_TOP
#define VIEW_ROWS NEO_LCD_VIEW_ROWS
#define ROW_HINT NEO_LCD_ROW_HINT
#define ROW_STATUS NEO_LCD_ROW_STATUS
#define ROW_INPUT NEO_LCD_ROW_INPUT

APPLET_HEADER_BEGIN
    APPLET_ID(NEO_LINK_APPLET_ID)
    APPLET_NAME("Neo Link Chat")
    APPLET_INFO("ESP32 buddy LLM link")
    APPLET_VERSION(NEO_LINK_APPLET_VERSION_MAJOR, NEO_LINK_APPLET_VERSION_MINOR,
                   NEO_LINK_APPLET_VERSION_REV_STR)
    APPLET_LANGUAGE_EN_US
    .fileCount = 2,
    .fileUsage = NEO_LINK_APPLET_FILE_USAGE,
APPLET_HEADER_END

typedef struct {
    char prompt[NEO_LINK_PROMPT_MAX + 1];
    char reply[NEO_LINK_REPLY_MAX + 1];
    char lines[MAX_LINES][COLS + 1];
    char status[32];
    uint8_t prompt_len;
    uint8_t line_count;
    uint8_t scroll;
    uint8_t usb;
    uint8_t sending;
    uint8_t composing;
} neo_link_state_t;

static neo_link_state_t s;

#if defined(__GNUC__)
_Static_assert(sizeof(neo_link_state_t) <= 700,
               "neo_link_state_t BSS too large — see neo_link_limits.h");
#endif

static const Key_e s_exit_keys[] = { KEY_ESC, KEY_ENTER, KEY_SEND, KEY_NONE };

static void set_status(const char *msg)
{
    if (!msg) {
        s.status[0] = '\0';
        return;
    }
    strncpy(s.status, msg, sizeof(s.status) - 1);
    s.status[sizeof(s.status) - 1] = '\0';
}

static void sanitize_reply(char *text, int len)
{
    int i;

    if (len < 0) {
        text[0] = '\0';
        return;
    }
    if (len > NEO_LINK_REPLY_MAX) {
        len = NEO_LINK_REPLY_MAX;
    }
    text[len] = '\0';
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c == '\n' || c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            text[i] = ' ';
        }
    }
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\0')) {
        text[--len] = '\0';
    }
}

static void wrap_reply(void)
{
    const char *p = s.reply;

    s.line_count = 0;
    memset(s.lines, 0, sizeof(s.lines));
    if (!p[0]) {
        return;
    }

    while (*p && s.line_count < MAX_LINES) {
        const char *line_start = p;
        const char *break_at = NULL;
        int col = 0;

        if (*p == '\n') {
            s.lines[s.line_count][0] = '\0';
            s.line_count++;
            p++;
            continue;
        }

        while (*p && *p != '\n' && col < COLS) {
            if (*p == ' ') {
                break_at = p;
            }
            p++;
            col++;
        }

        if (*p && *p != '\n' && break_at && break_at > line_start) {
            p = break_at;
        }

        {
            int n = (int)(p - line_start);
            if (n > COLS) {
                n = COLS;
            }
            if (n < 0) {
                n = 0;
            }
            memcpy(s.lines[s.line_count], line_start, (size_t)n);
            s.lines[s.line_count][n] = '\0';
            s.line_count++;
        }

        if (*p == ' ') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }

    if (s.scroll >= s.line_count) {
        s.scroll = s.line_count > 0 ? (uint8_t)(s.line_count - 1) : 0;
    }
}

static uint8_t max_scroll(void)
{
    if (s.line_count <= VIEW_ROWS) {
        return 0;
    }
    return (uint8_t)(s.line_count - VIEW_ROWS);
}

static void draw_viewport(void)
{
    uint8_t r;

    for (r = 0; r < VIEW_ROWS; r++) {
        uint8_t row = (uint8_t)(VIEW_TOP + r);
        ClearRowCols(row, COL_FIRST, COL_LAST);
        if ((uint8_t)(s.scroll + r) < s.line_count) {
            SetCursor(row, COL_FIRST, CURSOR_MODE_HIDE);
            PutString(s.lines[s.scroll + r]);
        }
    }
}

static void draw_chrome(void)
{
    ClearRowCols(ROW_TITLE, COL_FIRST, COL_LAST);
    ClearRowCols(ROW_USB, COL_FIRST, COL_LAST);
    ClearRowCols(ROW_HINT, COL_FIRST, COL_LAST);
    ClearRowCols(ROW_STATUS, COL_FIRST, COL_LAST);

    PutStringCentered(ROW_TITLE, "Neo Link Chat");
    PutStringCentered(ROW_USB, s.usb ? "USB: connected" : "USB: not connected");
    PutStringCentered(ROW_HINT, "Enter=ask Find=in (USB=OS)");

    if (s.status[0]) {
        PutStringCentered(ROW_STATUS, s.status);
    } else {
        PutStringCentered(ROW_STATUS, s.reply[0] ? "Buddy reply" : "No reply yet");
    }
}

/*
 * MSG_SETFOCUS: paint chrome only (O-3). No FileOpen, wrap_reply, or viewport
 * of mailbox data. Version banner on row 3 (viewport top).
 */
static void paint_focus(void)
{
    SetCursorMode(CURSOR_MODE_HIDE);
    draw_chrome();
    ClearRowCols(VIEW_TOP, COL_FIRST, COL_LAST);
    PutStringCentered(VIEW_TOP, "v0." NEO_LINK_APPLET_VERSION_MINOR_STR
                                   NEO_LINK_APPLET_VERSION_REV_STR
                                   "  Enter to ask");
    {
        uint8_t r;
        for (r = 1; r < VIEW_ROWS; r++) {
            ClearRowCols((uint8_t)(VIEW_TOP + r), COL_FIRST, COL_LAST);
        }
    }
}

static bool file_open_try(uint8_t file)
{
    int rc = FileOpen(file);
    if (rc) {
        return true;
    }
    rc = FileOpen(file);
    return rc != 0;
}

static void ensure_mailbox_files(void)
{
    static const char empty[1] = { '\0' };
    uint8_t n;

    for (n = NEO_LINK_MAILBOX_FILE_IN; n <= NEO_LINK_MAILBOX_FILE_OUT; n++) {
        if (file_open_try(n)) {
            (void)FileWriteBuffer(empty, 1);
            FileClose();
        }
    }
}

static bool read_mailbox_in(void)
{
    int rd;

    memset(s.reply, 0, sizeof(s.reply));
    if (!file_open_try(NEO_LINK_MAILBOX_FILE_IN)) {
        return false;
    }
    rd = FileReadBuffer(s.reply, NEO_LINK_REPLY_MAX);
    FileClose();
    if (rd < 0) {
        return false;
    }
    if (rd == 0) {
        rd = (int)strlen(s.reply);
    }
    if (rd <= 0) {
        return false;
    }
    if (rd > NEO_LINK_REPLY_MAX) {
        rd = NEO_LINK_REPLY_MAX;
    }
    sanitize_reply(s.reply, rd);
    if (s.reply[0] == '\0') {
        return false;
    }
    s.scroll = 0;
    return true;
}

static void draw_screen(void)
{
    SetCursorMode(CURSOR_MODE_HIDE);
    wrap_reply();
    draw_chrome();
    draw_viewport();
}

static void scroll_by(int delta)
{
    int next;

    if (s.line_count <= VIEW_ROWS) {
        return;
    }
    next = (int)s.scroll + delta;
    if (next < 0) {
        next = 0;
    }
    if (next > (int)max_scroll()) {
        next = (int)max_scroll();
    }
    if ((uint8_t)next == s.scroll) {
        return;
    }
    s.scroll = (uint8_t)next;
    draw_viewport();
    draw_chrome();
}

static void begin_prompt(void)
{
    KeyMod_e exit_key;

    s.composing = 1;
    s.prompt_len = 0;
    s.prompt[0] = '\0';
    set_status("Type question, Enter/Send");
    draw_chrome();
    ClearRowCols(ROW_INPUT, COL_FIRST, COL_LAST);
    SetCursor(ROW_INPUT, COL_FIRST, CURSOR_MODE_SHOW);
    exit_key = (KeyMod_e)TextBox(s.prompt, &s.prompt_len, NEO_LINK_PROMPT_MAX, s_exit_keys, false);
    SetCursorMode(CURSOR_MODE_HIDE);
    s.composing = 0;

    if ((exit_key & 0xFF) == KEY_ESC || s.prompt_len == 0) {
        set_status("Cancelled");
        draw_screen();
    }
}

static void try_send(void)
{
    int wait;

    if (s.sending || s.prompt_len == 0) {
        return;
    }
    if (!s.usb) {
        set_status("Plug in buddy USB first");
        draw_chrome();
        return;
    }

    s.sending = 1;
    set_status("Sending...");
    draw_chrome();
    if (neo_link_applet_send_chat(s.prompt)) {
        for (wait = 8; wait >= 1; wait--) {
            set_status("Waiting for buddy...");
            draw_chrome();
            if (IsKeyReady()) {
                DrainKeyBuffer();
                break;
            }
            SleepCentiseconds(100);
        }
        set_status("Sent - Find for reply");
        draw_chrome();
        SleepCentiseconds(50);
        if (read_mailbox_in()) {
            set_status("Reply ready");
            draw_screen();
        }
    } else {
        set_status("Send failed");
        draw_chrome();
    }
    s.sending = 0;
}

static void refresh_inbox(void)
{
    SleepCentiseconds(20);
    if (!read_mailbox_in()) {
        set_status("Mailbox empty");
        draw_screen();
        return;
    }
    s.scroll = 0;
    set_status("Reply ready");
    draw_screen();
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;

    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        ensure_mailbox_files();
        break;

    case MSG_SETFOCUS:
        paint_focus();
        break;

    case MSG_USB_PLUG:
        s.usb = 1;
        set_status("USB connected");
        paint_focus();
        break;

    case MSG_USB_UNPLUG:
        s.usb = 0;
        set_status("USB unplugged");
        paint_focus();
        break;

    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (s.composing) {
            break;
        }
        if (code == KEY_UP) {
            scroll_by(-1);
        } else if (code == KEY_DOWN) {
            scroll_by(1);
        } else if (code == KEY_FIND) {
            refresh_inbox();
        } else if (code == KEY_ENTER || code == KEY_SEND) {
            begin_prompt();
            if (s.prompt_len > 0) {
                try_send();
            }
        } else if (code == KEY_HOME) {
            s.scroll = 0;
            draw_viewport();
            draw_chrome();
        } else if (code == KEY_END) {
            s.scroll = max_scroll();
            draw_viewport();
            draw_chrome();
        }
        break;
    }

    default:
        break;
    }
}
