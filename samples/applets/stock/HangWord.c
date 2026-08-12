/**
 * @file HangWord.c
 * @brief Guess the word — hangman with hints and streaks.
 *
 * Type A–Z to guess. Tab = hint (costs a miss).
 * Enter / Find = next word. Clear File = reset score.
 */
#include "os3k.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B9
#define MAX_WORD 10
#define MAX_MISS 6
#define WORD_COUNT 40

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Hang Word")
    APPLET_INFO("Guess the word — hints and streaks")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
APPLET_HEADER_END

typedef struct {
    uint16_t rng;
    uint8_t last_idx;
    char word[MAX_WORD];
    uint32_t guessed; /* bit 0=A … bit 25=Z */
    uint8_t misses;
    uint8_t won;
    uint8_t lost;
    uint16_t wins;
    uint16_t losses;
    uint16_t streak;
    uint16_t best_streak;
    char toast[18];
} hang_state_t;

static hang_state_t s;

static const char k_words[WORD_COUNT][MAX_WORD] = {
    "APPLE", "NEO", "BUDDY", "SNAKE", "WRITE", "FOCUS", "BOARD", "PIXEL",
    "CLOUD", "FLASH", "DRILL", "TASK", "SCRIPT", "TREE", "MATH", "CARD",
    "KEYBOARD", "PORTAL", "BACKUP", "DEVICE", "LETTER", "PUZZLE", "GAMES",
    "SMART", "ALPHA", "NOTES", "SCORE", "STREAK", "LUCKY", "QUEST", "BRAIN",
    "ORBIT", "QUILL", "RIVER", "STORM", "TIGER", "UNION", "VIVID", "ZEBRA",
    "HAPPY"
};

static uint16_t rng_next(void)
{
    s.rng ^= (uint16_t)(GetUptimeMilliseconds() & 0xFFFF);
    s.rng = (uint16_t)(s.rng * 1103515245u + 12345u);
    return s.rng;
}

static int letter_bit(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    if (c < 'A' || c > 'Z') {
        return -1;
    }
    return (int)(c - 'A');
}

static void set_toast(const char *msg)
{
    strncpy(s.toast, msg ? msg : "", sizeof(s.toast) - 1);
    s.toast[sizeof(s.toast) - 1] = '\0';
}

static int word_complete(void)
{
    uint8_t i;
    int bit;
    for (i = 0; s.word[i]; i++) {
        bit = letter_bit(s.word[i]);
        if (bit < 0) {
            continue;
        }
        if ((s.guessed & (1u << bit)) == 0) {
            return 0;
        }
    }
    return 1;
}

static void new_round(void)
{
    uint8_t idx;
    uint8_t tries = 0;
    do {
        idx = (uint8_t)stock_urem16(rng_next(), WORD_COUNT);
        tries++;
    } while (idx == s.last_idx && WORD_COUNT > 1 && tries < 8);
    s.last_idx = idx;
    strncpy(s.word, k_words[idx], MAX_WORD - 1);
    s.word[MAX_WORD - 1] = '\0';
    s.guessed = 0;
    s.misses = 0;
    s.won = 0;
    s.lost = 0;
    set_toast("");
}

static void draw_gallows(void)
{
    const char *l2 = "  |";
    const char *l3 = "  |";
    const char *l4 = "  |";

    if (s.misses >= 1) {
        l2 = "  |  O";
    }
    if (s.misses >= 4) {
        l3 = "  | /|\\";
    } else if (s.misses >= 3) {
        l3 = "  | /|";
    } else if (s.misses >= 2) {
        l3 = "  |  |";
    }
    if (s.misses >= 6) {
        l4 = "  | / \\";
    } else if (s.misses >= 5) {
        l4 = "  | /";
    }

    SetCursor(2, 1, CURSOR_MODE_HIDE);
    PutStringRaw("  +--+");
    SetCursor(3, 1, CURSOR_MODE_HIDE);
    PutStringRaw(l2);
    SetCursor(4, 1, CURSOR_MODE_HIDE);
    PutStringRaw(l3);
    SetCursor(5, 1, CURSOR_MODE_HIDE);
    PutStringRaw(l4);
}

static void draw(void)
{
    char line[40];
    char a[6];
    char b[6];
    char mask[24];
    char alpha[28];
    uint8_t i;
    uint8_t n;
    int bit;

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();

    stock_u32_to_str(s.wins, a, sizeof(a));
    stock_u32_to_str(s.losses, b, sizeof(b));
    strncpy(line, "Hang Word  ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    strncat(line, "-", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    if (s.streak > 1) {
        strncat(line, "  *", sizeof(line) - strlen(line) - 1);
        stock_u32_to_str(s.streak, a, sizeof(a));
        strncat(line, a, sizeof(line) - strlen(line) - 1);
    }
    PutStringCentered(1, line);

    draw_gallows();

    mask[0] = '\0';
    for (i = 0; s.word[i] && i < MAX_WORD; i++) {
        bit = letter_bit(s.word[i]);
        if (bit >= 0 && (s.guessed & (1u << bit))) {
            char ch[2] = { s.word[i], '\0' };
            strncat(mask, ch, sizeof(mask) - strlen(mask) - 1);
        } else if (s.lost) {
            char ch[2] = { s.word[i], '\0' };
            strncat(mask, ch, sizeof(mask) - strlen(mask) - 1);
        } else {
            strncat(mask, "_", sizeof(mask) - strlen(mask) - 1);
        }
        strncat(mask, " ", sizeof(mask) - strlen(mask) - 1);
    }
    SetCursor(3, 12, CURSOR_MODE_HIDE);
    PutStringRaw(mask);

    stock_u32_to_str(s.misses, a, sizeof(a));
    strncpy(line, "Lives ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    stock_u32_to_str((uint32_t)(MAX_MISS - s.misses), b, sizeof(b));
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, "/", sizeof(line) - strlen(line) - 1);
    stock_u32_to_str(MAX_MISS, b, sizeof(b));
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    SetCursor(4, 12, CURSOR_MODE_HIDE);
    PutStringRaw(line);

    /* Compact A–Z: guessed letters shown, others as dots. */
    n = 0;
    for (i = 0; i < 26 && n < 26; i++) {
        if (s.guessed & (1u << i)) {
            alpha[n++] = (char)('A' + i);
        } else {
            alpha[n++] = '.';
        }
    }
    alpha[n] = '\0';
    SetCursor(6, 1, CURSOR_MODE_HIDE);
    PutStringRaw(alpha);

    if (s.toast[0] && !s.won && !s.lost) {
        PutStringCentered(7, s.toast);
    } else if (s.won) {
        if (s.streak > s.best_streak) {
            PutStringCentered(7, "New streak!");
        } else {
            PutStringCentered(7, "Nice!");
        }
    } else if (s.lost) {
        strncpy(line, "It was ", sizeof(line) - 1);
        line[sizeof(line) - 1] = '\0';
        strncat(line, s.word, sizeof(line) - strlen(line) - 1);
        PutStringCentered(7, line);
    }

    if (s.won) {
        PutStringCentered(8, "Enter=next word");
    } else if (s.lost) {
        PutStringCentered(8, "Enter=try again");
    } else {
        PutStringCentered(8, "Type letter  Tab=hint");
    }
}

static void on_win(void)
{
    s.won = 1;
    s.wins++;
    s.streak++;
    if (s.streak > s.best_streak) {
        s.best_streak = s.streak;
    }
    set_toast("You got it!");
}

static void on_lose(void)
{
    s.lost = 1;
    s.losses++;
    s.streak = 0;
}

static void guess_letter(char c)
{
    int bit;
    uint8_t i;
    int hit = 0;

    if (s.won || s.lost) {
        return;
    }
    bit = letter_bit(c);
    if (bit < 0) {
        return;
    }
    if (s.guessed & (1u << bit)) {
        set_toast("Already tried");
        draw();
        return;
    }
    s.guessed |= (1u << bit);
    for (i = 0; s.word[i]; i++) {
        if (letter_bit(s.word[i]) == bit) {
            hit = 1;
        }
    }
    if (!hit) {
        s.misses++;
        set_toast("Miss");
        if (s.misses >= MAX_MISS) {
            on_lose();
        }
    } else {
        set_toast("Hit!");
        if (word_complete()) {
            on_win();
        }
    }
    draw();
}

static void use_hint(void)
{
    uint8_t i;
    int bit;
    if (s.won || s.lost) {
        return;
    }
    if (s.misses + 1 >= MAX_MISS) {
        set_toast("Too risky");
        draw();
        return;
    }
    for (i = 0; s.word[i]; i++) {
        bit = letter_bit(s.word[i]);
        if (bit >= 0 && (s.guessed & (1u << bit)) == 0) {
            s.guessed |= (1u << bit);
            s.misses++;
            set_toast("Hint used");
            if (word_complete()) {
                on_win();
            } else if (s.misses >= MAX_MISS) {
                on_lose();
            }
            draw();
            return;
        }
    }
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.rng = 7;
        s.last_idx = 255;
        new_round();
        break;
    case MSG_SETFOCUS:
        draw();
        break;
    case MSG_KILLFOCUS:
        break;
    case MSG_CHAR:
        guess_letter((char)(param & 0xFF));
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_ENTER || code == KEY_SEND || code == KEY_FIND) {
            new_round();
            draw();
        } else if (code == KEY_TAB) {
            use_hint();
        } else if (code == KEY_CLEAR_FILE) {
            s.wins = 0;
            s.losses = 0;
            s.streak = 0;
            s.best_streak = 0;
            new_round();
            draw();
        }
        break;
    }
    default:
        break;
    }
}
