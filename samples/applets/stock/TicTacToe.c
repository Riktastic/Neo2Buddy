/**
 * @file TicTacToe.c
 * @brief Tic-tac-toe vs Neo — two difficulties, number keys, scores.
 *
 * Arrows move cursor. Enter / Space places X.
 * Keys 1–9 place directly (keypad layout).
 * Tab = Easy/Hard. Find = new game. Clear File = reset score.
 */
#include "os3k.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1BA
#define CELL_EMPTY 0
#define CELL_X 1
#define CELL_O 2
#define STATE_PLAY 0
#define STATE_WIN_X 1
#define STATE_WIN_O 2
#define STATE_DRAW 3
#define DIFF_EASY 0
#define DIFF_HARD 1

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Tic Tac Toe")
    APPLET_INFO("Beat Neo — Easy or Hard AI")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
APPLET_HEADER_END

typedef struct {
    uint8_t board[9];
    uint8_t cursor;
    uint8_t state;
    uint8_t diff;
    uint8_t last_ai;
    uint16_t wins;
    uint16_t losses;
    uint16_t draws;
    uint16_t rng;
} ttt_state_t;

static ttt_state_t s;

static const uint8_t k_wins[8][3] = {
    {0, 1, 2}, {3, 4, 5}, {6, 7, 8},
    {0, 3, 6}, {1, 4, 7}, {2, 5, 8},
    {0, 4, 8}, {2, 4, 6}
};

static uint16_t rng_next(void)
{
    s.rng ^= (uint16_t)(GetUptimeMilliseconds() & 0xFFFF);
    s.rng = (uint16_t)(s.rng * 1103515245u + 12345u);
    return s.rng;
}

static int winner(void)
{
    uint8_t i;
    for (i = 0; i < 8; i++) {
        uint8_t a = s.board[k_wins[i][0]];
        uint8_t b = s.board[k_wins[i][1]];
        uint8_t c = s.board[k_wins[i][2]];
        if (a && a == b && b == c) {
            return (int)a;
        }
    }
    return 0;
}

static int board_full(void)
{
    uint8_t i;
    for (i = 0; i < 9; i++) {
        if (s.board[i] == CELL_EMPTY) {
            return 0;
        }
    }
    return 1;
}

static void refresh_state(void)
{
    int w = winner();
    if (w == CELL_X) {
        s.state = STATE_WIN_X;
        s.wins++;
    } else if (w == CELL_O) {
        s.state = STATE_WIN_O;
        s.losses++;
    } else if (board_full()) {
        s.state = STATE_DRAW;
        s.draws++;
    } else {
        s.state = STATE_PLAY;
    }
}

static int find_win_move(uint8_t who)
{
    uint8_t i;
    for (i = 0; i < 9; i++) {
        if (s.board[i] == CELL_EMPTY) {
            s.board[i] = who;
            if (winner() == (int)who) {
                s.board[i] = CELL_EMPTY;
                return (int)i;
            }
            s.board[i] = CELL_EMPTY;
        }
    }
    return -1;
}

static int random_empty(void)
{
    uint8_t prefs[9];
    uint8_t n = 0;
    uint8_t i;
    for (i = 0; i < 9; i++) {
        if (s.board[i] == CELL_EMPTY) {
            prefs[n++] = i;
        }
    }
    if (!n) {
        return -1;
    }
    return (int)prefs[stock_urem16(rng_next(), n)];
}

static void ai_move(void)
{
    int m = -1;
    uint8_t prefs[4];
    uint8_t n = 0;
    uint8_t i;
    static const uint8_t corners[4] = {0, 2, 6, 8};
    static const uint8_t sides[4] = {1, 3, 5, 7};

    if (s.state != STATE_PLAY) {
        return;
    }

    if (s.diff == DIFF_EASY) {
        /* Easy: sometimes play randomly, sometimes block. */
        if (stock_urem16(rng_next(), 100) < 55) {
            m = random_empty();
        } else {
            m = find_win_move(CELL_O);
            if (m < 0) {
                m = find_win_move(CELL_X);
            }
            if (m < 0) {
                m = random_empty();
            }
        }
    } else {
        m = find_win_move(CELL_O);
        if (m < 0) {
            m = find_win_move(CELL_X);
        }
        if (m < 0 && s.board[4] == CELL_EMPTY) {
            m = 4;
        }
        if (m < 0) {
            for (i = 0; i < 4; i++) {
                if (s.board[corners[i]] == CELL_EMPTY) {
                    prefs[n++] = corners[i];
                }
            }
            if (n) {
                m = (int)prefs[stock_urem16(rng_next(), n)];
            }
        }
        if (m < 0) {
            n = 0;
            for (i = 0; i < 4; i++) {
                if (s.board[sides[i]] == CELL_EMPTY) {
                    prefs[n++] = sides[i];
                }
            }
            if (n) {
                m = (int)prefs[stock_urem16(rng_next(), n)];
            }
        }
        if (m < 0) {
            m = random_empty();
        }
    }

    if (m >= 0) {
        s.board[m] = CELL_O;
        s.last_ai = (uint8_t)m;
        refresh_state();
    }
}

static void new_game(void)
{
    memset(s.board, 0, sizeof(s.board));
    s.cursor = 4;
    s.state = STATE_PLAY;
    s.last_ai = 255;
}

static char cell_char(uint8_t idx)
{
    if (s.board[idx] == CELL_X) {
        return 'X';
    }
    if (s.board[idx] == CELL_O) {
        return 'O';
    }
    if (s.state == STATE_PLAY && s.cursor == idx) {
        return '+';
    }
    return ' ';
}

static void draw(void)
{
    char line[40];
    char a[6];
    char b[6];
    char c[6];
    uint8_t row;

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();

    stock_u32_to_str(s.wins, a, sizeof(a));
    stock_u32_to_str(s.losses, b, sizeof(b));
    stock_u32_to_str(s.draws, c, sizeof(c));
    strncpy(line, "You ", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, a, sizeof(line) - strlen(line) - 1);
    strncat(line, "  Neo ", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, "  =", sizeof(line) - strlen(line) - 1);
    strncat(line, c, sizeof(line) - strlen(line) - 1);
    PutStringCentered(1, line);

    strncpy(line, s.diff == DIFF_HARD ? "[Hard]" : "[Easy]", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    PutStringCentered(2, line);

    for (row = 0; row < 3; row++) {
        uint8_t base = (uint8_t)(row * 3);
        line[0] = ' ';
        line[1] = cell_char(base);
        line[2] = ' ';
        line[3] = '|';
        line[4] = ' ';
        line[5] = cell_char((uint8_t)(base + 1));
        line[6] = ' ';
        line[7] = '|';
        line[8] = ' ';
        line[9] = cell_char((uint8_t)(base + 2));
        line[10] = '\0';
        PutStringCentered((uint8_t)(3 + row * 2), line);
        if (row < 2) {
            PutStringCentered((uint8_t)(4 + row * 2), "---+---+---");
        }
    }

    if (s.state == STATE_WIN_X) {
        PutStringCentered(8, "You win! Find=again");
    } else if (s.state == STATE_WIN_O) {
        PutStringCentered(8, "Neo wins. Find=again");
    } else if (s.state == STATE_DRAW) {
        PutStringCentered(8, "Draw! Find=again");
    } else {
        PutStringCentered(8, "1-9 or +Enter  Tab=AI");
    }
}

static void move_cursor(int8_t dx, int8_t dy)
{
    uint8_t x = (uint8_t)stock_urem16(s.cursor, 3);
    uint8_t y = (uint8_t)stock_udiv32(s.cursor, 3);
    if (dx < 0 && x > 0) {
        x--;
    } else if (dx > 0 && x < 2) {
        x++;
    }
    if (dy < 0 && y > 0) {
        y--;
    } else if (dy > 0 && y < 2) {
        y++;
    }
    s.cursor = (uint8_t)(y * 3 + x);
}

static void place_at(uint8_t idx)
{
    if (s.state != STATE_PLAY || idx > 8) {
        return;
    }
    if (s.board[idx] != CELL_EMPTY) {
        return;
    }
    s.cursor = idx;
    s.board[idx] = CELL_X;
    refresh_state();
    if (s.state == STATE_PLAY) {
        ai_move();
    }
}

static void place(void)
{
    place_at(s.cursor);
}

static int key_to_cell(uint8_t code)
{
    /* Phone keypad: 7 8 9 / 4 5 6 / 1 2 3 */
    if (code == KEY_7) return 0;
    if (code == KEY_8) return 1;
    if (code == KEY_9) return 2;
    if (code == KEY_4) return 3;
    if (code == KEY_5) return 4;
    if (code == KEY_6) return 5;
    if (code == KEY_1) return 6;
    if (code == KEY_2) return 7;
    if (code == KEY_3) return 8;
    return -1;
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.rng = 11;
        s.diff = DIFF_EASY;
        s.last_ai = 255;
        new_game();
        break;
    case MSG_SETFOCUS:
        draw();
        break;
    case MSG_KILLFOCUS:
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        int cell = key_to_cell(code);
        if (code == KEY_ENTER || code == KEY_SEND || code == KEY_SPACE ||
            code == KEY_SPELL_CHECK) {
            if (s.state != STATE_PLAY) {
                new_game();
            } else {
                place();
            }
            draw();
        } else if (cell >= 0) {
            if (s.state != STATE_PLAY) {
                new_game();
            }
            place_at((uint8_t)cell);
            draw();
        } else if (code == KEY_UP) {
            move_cursor(0, -1);
            draw();
        } else if (code == KEY_DOWN) {
            move_cursor(0, 1);
            draw();
        } else if (code == KEY_LEFT) {
            move_cursor(-1, 0);
            draw();
        } else if (code == KEY_RIGHT) {
            move_cursor(1, 0);
            draw();
        } else if (code == KEY_TAB) {
            s.diff = s.diff == DIFF_HARD ? DIFF_EASY : DIFF_HARD;
            draw();
        } else if (code == KEY_FIND) {
            new_game();
            draw();
        } else if (code == KEY_CLEAR_FILE) {
            s.wins = 0;
            s.losses = 0;
            s.draws = 0;
            new_game();
            draw();
        }
        break;
    }
    default:
        break;
    }
}
