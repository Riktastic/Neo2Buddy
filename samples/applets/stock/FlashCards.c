/**
 * @file FlashCards.c
 * @brief Flashcards with reveal / reverse / type modes (+ hints).
 *
 * Deck: front|back lines (portal can push many named sets; Neo holds one).
 *
 * Tab = cycle mode: Show → Reverse → Type → Type↔
 * Show/Reverse: Space reveal, Y/Enter=got, N/Del=miss, arrows move
 * Type modes: type the answer, Enter check, Find=hint, Esc=clear
 * Find (show modes) = shuffle   Clear File = EN→NL starter
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B6
#define MAX_CARDS 16
#define SIDE_LEN 24
#define VIEW_IO 480
#define STARTER_CARDS 16
#define MODE_SHOW 0
#define MODE_REV 1
#define MODE_TYPE 2
#define MODE_TYPE_REV 3
#define MODE_COUNT 4

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Flash Cards")
    APPLET_INFO("EN/NL cards — show reverse type")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 1024,
APPLET_HEADER_END

typedef struct {
    char front[MAX_CARDS][SIDE_LEN];
    char back[MAX_CARDS][SIDE_LEN];
    uint8_t count;
    uint8_t index;
    uint8_t revealed;
    uint8_t order[MAX_CARDS];
    uint8_t mode;
    uint8_t hint_len;
    uint16_t got;
    uint16_t miss;
    char typed[SIDE_LEN];
    uint8_t typed_len;
    char toast[20];
} cards_state_t;

static cards_state_t s;
static char s_io[VIEW_IO];

static const char *mode_name(uint8_t mode)
{
    if (mode == MODE_REV) {
        return "Reverse";
    }
    if (mode == MODE_TYPE) {
        return "Type";
    }
    if (mode == MODE_TYPE_REV) {
        return "Type Rev";
    }
    return "Show";
}

static int mode_is_type(void)
{
    return s.mode == MODE_TYPE || s.mode == MODE_TYPE_REV;
}

static int mode_is_rev(void)
{
    return s.mode == MODE_REV || s.mode == MODE_TYPE_REV;
}

static void set_toast(const char *msg)
{
    strncpy(s.toast, msg ? msg : "", sizeof(s.toast) - 1);
    s.toast[sizeof(s.toast) - 1] = '\0';
}

static void seed_starter(void)
{
    static const char fronts[STARTER_CARDS][SIDE_LEN] = {
        "hello", "goodbye", "please", "thank you",
        "yes", "no", "good morning", "how are you?",
        "I", "you", "water", "bread",
        "milk", "house", "friend", "today"
    };
    static const char backs[STARTER_CARDS][SIDE_LEN] = {
        "hallo", "tot ziens", "alsjeblieft", "dank je",
        "ja", "nee", "goedemorgen", "hoe gaat het?",
        "ik", "jij", "water", "brood",
        "melk", "huis", "vriend", "vandaag"
    };
    uint8_t i;
    s.count = STARTER_CARDS;
    for (i = 0; i < STARTER_CARDS; i++) {
        strncpy(s.front[i], fronts[i], SIDE_LEN - 1);
        s.front[i][SIDE_LEN - 1] = '\0';
        strncpy(s.back[i], backs[i], SIDE_LEN - 1);
        s.back[i][SIDE_LEN - 1] = '\0';
        s.order[i] = i;
    }
    s.index = 0;
    s.revealed = 0;
    s.hint_len = 0;
    s.typed_len = 0;
    s.typed[0] = '\0';
}

static void reset_order(void)
{
    uint8_t i;
    for (i = 0; i < s.count; i++) {
        s.order[i] = i;
    }
}

static void shuffle_order(void)
{
    uint8_t i;
    uint16_t seed = (uint16_t)(GetUptimeMilliseconds() & 0xFFFF);
    if (s.count < 2) {
        return;
    }
    for (i = (uint8_t)(s.count - 1); i > 0; i--) {
        uint8_t j;
        uint8_t tmp;
        seed = (uint16_t)(seed * 1103515245u + 12345u);
        j = (uint8_t)stock_urem16(seed, (uint16_t)(i + 1));
        tmp = s.order[i];
        s.order[i] = s.order[j];
        s.order[j] = tmp;
    }
    s.index = 0;
    s.revealed = 0;
    s.hint_len = 0;
    s.typed_len = 0;
    s.typed[0] = '\0';
}

static void trim_inplace(char *text)
{
    char *start = text;
    size_t n;
    while (*start == ' ' || *start == '\t' || *start == '\r') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }
    n = strlen(text);
    while (n > 0 && (text[n - 1] == ' ' || text[n - 1] == '\t' || text[n - 1] == '\r')) {
        text[--n] = '\0';
    }
}

static int split_card(char *line, char *front, char *back)
{
    char *sep = strchr(line, '|');
    if (!sep) {
        sep = strchr(line, '\t');
    }
    if (!sep || sep == line || !sep[1]) {
        return 0;
    }
    *sep = '\0';
    strncpy(front, line, SIDE_LEN - 1);
    front[SIDE_LEN - 1] = '\0';
    strncpy(back, sep + 1, SIDE_LEN - 1);
    back[SIDE_LEN - 1] = '\0';
    trim_inplace(front);
    trim_inplace(back);
    return front[0] && back[0];
}

static void save_deck(void);

static void load_deck(void)
{
    char *p;
    char *line;
    uint8_t keep_mode = s.mode;
    memset(&s, 0, sizeof(s));
    s.mode = keep_mode;
    memset(s_io, 0, sizeof(s_io));
    if (!FileOpen(1)) {
        seed_starter();
        save_deck();
        return;
    }
    (void)FileReadBuffer(s_io, (uint16_t)(sizeof(s_io) - 1));
    FileClose();
    p = s_io;
    while (*p && s.count < MAX_CARDS) {
        line = p;
        while (*p && *p != '\n') {
            p++;
        }
        if (*p == '\n') {
            *p++ = '\0';
        }
        if (line[0] == '#' || line[0] == '\0') {
            continue;
        }
        if (split_card(line, s.front[s.count], s.back[s.count])) {
            s.count++;
        }
    }
    if (s.count == 0) {
        seed_starter();
        save_deck();
    } else {
        reset_order();
    }
}

static void save_deck(void)
{
    uint8_t i;
    size_t off = 0;
    memset(s_io, 0, sizeof(s_io));
    for (i = 0; i < s.count && off + SIDE_LEN * 2 + 2 < sizeof(s_io); i++) {
        strncpy(&s_io[off], s.front[i], SIDE_LEN - 1);
        off += strlen(&s_io[off]);
        s_io[off++] = '|';
        strncpy(&s_io[off], s.back[i], SIDE_LEN - 1);
        off += strlen(&s_io[off]);
        s_io[off++] = '\n';
    }
    if (FileOpen(1)) {
        FileWriteBuffer(s_io, (uint16_t)sizeof(s_io));
        FileClose();
    }
}

static uint8_t cur_card(void)
{
    if (s.count == 0) {
        return 0;
    }
    return s.order[s.index % s.count];
}

static const char *prompt_side(uint8_t card)
{
    return mode_is_rev() ? s.back[card] : s.front[card];
}

static const char *answer_side(uint8_t card)
{
    return mode_is_rev() ? s.front[card] : s.back[card];
}

static void clear_type_state(void)
{
    s.typed_len = 0;
    s.typed[0] = '\0';
    s.hint_len = 0;
    s.revealed = 0;
    set_toast("");
}

static void next_card(int dir)
{
    if (s.count == 0) {
        return;
    }
    if (dir > 0) {
        s.index++;
        if (s.index >= s.count) {
            s.index = 0;
        }
    } else if (dir < 0) {
        if (s.index == 0) {
            s.index = (uint8_t)(s.count - 1);
        } else {
            s.index--;
        }
    }
    clear_type_state();
}

static void draw_screen(void)
{
    char line[41];
    char a[6], b[6], c[6];
    uint8_t card;
    const char *prompt;
    const char *answer;
    size_t alen;

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();
    if (s.count == 0) {
        PutStringCentered(2, "No cards");
        PutStringCentered(4, "Clear File = EN to NL");
        PutStringCentered(8, "or push a set from buddy");
        return;
    }
    card = cur_card();
    prompt = prompt_side(card);
    answer = answer_side(card);
    alen = strlen(answer);

    stock_u32_to_str((uint32_t)(s.index + 1), a, sizeof(a));
    stock_u32_to_str(s.count, b, sizeof(b));
    strncpy(line, mode_name(s.mode), sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, "  ", sizeof(line) - strlen(line) - 1);
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    strncat(line, "/", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    if (s.got || s.miss) {
        strncat(line, " +", sizeof(line) - strlen(line) - 1);
        stock_u32_to_str(s.got, c, sizeof(c));
        strncat(line, c, sizeof(line) - strlen(line) - 1);
        strncat(line, "/-", sizeof(line) - strlen(line) - 1);
        stock_u32_to_str(s.miss, c, sizeof(c));
        strncat(line, c, sizeof(line) - strlen(line) - 1);
    }
    PutStringCentered(1, line);

    PutStringCentered(3, mode_is_rev() ? "NL / prompt" : "EN / prompt");
    PutStringCentered(4, prompt);

    ClearRowCols(6, 1, 40);
    ClearRowCols(7, 1, 40);

    if (mode_is_type()) {
        char hint[SIDE_LEN];
        uint8_t i;
        if (s.revealed) {
            PutStringCentered(6, "Answer");
            PutStringCentered(7, answer);
        } else {
            for (i = 0; i < s.hint_len && i < alen && i < SIDE_LEN - 1; i++) {
                hint[i] = answer[i];
            }
            for (; i < alen && i < SIDE_LEN - 1; i++) {
                hint[i] = '_';
            }
            hint[i] = '\0';
            PutStringCentered(6, s.hint_len ? hint : "Type answer");
            SetCursor(7, 1, CURSOR_MODE_SHOW);
            PutStringRaw(s.typed[0] ? s.typed : "_");
        }
        if (s.toast[0]) {
            PutStringCentered(8, s.toast);
        } else {
            PutStringCentered(8, "Enter=check Find=hint Tab");
        }
        return;
    }

    if (s.revealed) {
        PutStringCentered(6, mode_is_rev() ? "EN" : "NL");
        PutStringCentered(7, answer);
        PutStringCentered(8, "Y=got N=miss Tab=mode");
    } else {
        PutStringCentered(6, "(think, then Space)");
        PutStringCentered(8, "Space=reveal Tab=mode");
    }
}

static void grade_reveal(int got_it)
{
    if (!s.revealed) {
        s.revealed = 1;
        draw_screen();
        return;
    }
    if (got_it) {
        s.got++;
    } else {
        s.miss++;
    }
    next_card(1);
    draw_screen();
}

static void check_typed(void)
{
    const char *answer = answer_side(cur_card());
    if (s.revealed) {
        next_card(1);
        draw_screen();
        return;
    }
    if (!s.typed_len) {
        set_toast("Type something");
        draw_screen();
        return;
    }
    if (strcmp(s.typed, answer) == 0) {
        s.got++;
        set_toast("Correct!");
        ClearRowCols(8, 1, 40);
        PutStringCentered(8, "Correct!");
        SleepCentiseconds(35);
        next_card(1);
        draw_screen();
    } else {
        s.miss++;
        s.revealed = 1;
        set_toast("Miss — Enter=next");
        draw_screen();
    }
}

static void add_hint(void)
{
    const char *answer = answer_side(cur_card());
    size_t alen = strlen(answer);
    if (s.revealed || alen == 0) {
        return;
    }
    if (s.hint_len < alen) {
        s.hint_len++;
    }
    /* Prefill typed with the revealed hint prefix when empty or matching. */
    if (s.typed_len <= s.hint_len) {
        uint8_t i;
        for (i = 0; i < s.hint_len && i < SIDE_LEN - 1; i++) {
            s.typed[i] = answer[i];
        }
        s.typed[i] = '\0';
        s.typed_len = i;
    }
    draw_screen();
}

static void on_type_char(char ch)
{
    if (s.revealed) {
        return;
    }
    if (ch < 0x20 || ch > 0x7e) {
        return;
    }
    if (s.typed_len + 1 >= SIDE_LEN) {
        return;
    }
    s.typed[s.typed_len++] = ch;
    s.typed[s.typed_len] = '\0';
    set_toast("");
    draw_screen();
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        load_deck();
        break;
    case MSG_SETFOCUS:
        load_deck();
        clear_type_state();
        draw_screen();
        break;
    case MSG_KILLFOCUS:
        break;
    case MSG_CHAR: {
        char ch = (char)(param & 0xFF);
        if (mode_is_type()) {
            on_type_char(ch);
            break;
        }
        if (ch == 'y' || ch == 'Y') {
            grade_reveal(1);
        } else if (ch == 'n' || ch == 'N') {
            grade_reveal(0);
        }
        break;
    }
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_TAB) {
            s.mode++;
            if (s.mode >= MODE_COUNT) {
                s.mode = 0;
            }
            clear_type_state();
            set_toast("");
            draw_screen();
            break;
        }
        if (code == KEY_CLEAR_FILE) {
            seed_starter();
            s.got = 0;
            s.miss = 0;
            clear_type_state();
            save_deck();
            draw_screen();
            break;
        }
        if (mode_is_type()) {
            if (code == KEY_ENTER || code == KEY_SEND) {
                check_typed();
            } else if (code == KEY_FIND) {
                add_hint();
            } else if (code == KEY_ESC) {
                clear_type_state();
                draw_screen();
            } else if (code == KEY_BACKSPACE || code == KEY_DELETE) {
                if (s.typed_len > 0) {
                    s.typed_len--;
                    s.typed[s.typed_len] = '\0';
                    draw_screen();
                }
            } else if (code == KEY_RIGHT || code == KEY_DOWN) {
                next_card(1);
                draw_screen();
            } else if (code == KEY_LEFT || code == KEY_UP) {
                next_card(-1);
                draw_screen();
            }
            break;
        }
        if (code == KEY_SPACE || code == KEY_SPELL_CHECK) {
            if (!s.revealed) {
                s.revealed = 1;
                draw_screen();
            } else {
                grade_reveal(1);
            }
        } else if (code == KEY_ENTER || code == KEY_SEND) {
            grade_reveal(1);
        } else if (code == KEY_DELETE || code == KEY_BACKSPACE) {
            grade_reveal(0);
        } else if (code == KEY_RIGHT || code == KEY_DOWN) {
            next_card(1);
            draw_screen();
        } else if (code == KEY_LEFT || code == KEY_UP) {
            next_card(-1);
            draw_screen();
        } else if (code == KEY_FIND) {
            shuffle_order();
            ClearRowCols(8, 1, 40);
            PutStringCentered(8, "Shuffled");
            SleepCentiseconds(40);
            draw_screen();
        }
        break;
    }
    default:
        break;
    }
}
