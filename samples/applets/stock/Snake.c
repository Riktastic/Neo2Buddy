/**
 * @file Snake.c
 * @brief Classic snake — grows, speeds up, high score.
 *
 * Enter = start / again
 * Arrows = steer (press same way again to nudge a step)
 * Space = pause   Clear File = reset best
 * Auto-ticks on MSG_IDLE; speed rises with score.
 */
#include "os3k.h"
#include "stock_os3k_files.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B8
#define GRID_W 20
#define GRID_H 5
#define MAX_LEN 60
#define TICK_SLOW 34
#define TICK_FAST 12
#define MODE_IDLE 0
#define MODE_RUN 1
#define MODE_DEAD 2

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Snake")
    APPLET_INFO("Eat stars, grow fast, beat your best")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
    .fileCount = 1,
    .fileUsage = 32,
APPLET_HEADER_END

typedef struct {
    uint8_t body[MAX_LEN]; /* (y << 5) | x */
    uint8_t len;
    int8_t dx;
    int8_t dy;
    int8_t pending_dx;
    int8_t pending_dy;
    uint8_t food;
    uint8_t mode;
    uint8_t paused;
    uint8_t ate_flash;
    uint16_t score;
    uint16_t best;
    uint16_t rng;
    uint32_t last_tick;
    uint8_t dirty_best;
} snake_state_t;

static snake_state_t s;

static uint8_t pack(uint8_t x, uint8_t y)
{
    return (uint8_t)((y << 5) | x);
}

static uint8_t unpack_x(uint8_t p)
{
    return (uint8_t)(p & 0x1F);
}

static uint8_t unpack_y(uint8_t p)
{
    return (uint8_t)(p >> 5);
}

static uint16_t rng_next(void)
{
    s.rng ^= (uint16_t)(GetUptimeMilliseconds() & 0xFFFF);
    s.rng = (uint16_t)(s.rng * 1103515245u + 12345u);
    return s.rng;
}

static uint16_t tick_delay(void)
{
    uint16_t d;
    /* Faster every 3 points, floor at TICK_FAST. */
    if (s.score >= 30) {
        return TICK_FAST;
    }
    d = (uint16_t)(TICK_SLOW - s.score);
    if (d < TICK_FAST) {
        d = TICK_FAST;
    }
    return d;
}

static int body_hits(uint8_t x, uint8_t y, uint8_t skip_tail)
{
    uint8_t i;
    uint8_t last = s.len;
    uint8_t p = pack(x, y);
    if (skip_tail && last > 0) {
        last--;
    }
    for (i = 0; i < last; i++) {
        if (s.body[i] == p) {
            return 1;
        }
    }
    return 0;
}

static void place_food(void)
{
    uint8_t tries = 0;
    uint8_t x;
    uint8_t y;
    do {
        x = (uint8_t)stock_urem16(rng_next(), GRID_W);
        y = (uint8_t)stock_urem16(rng_next(), GRID_H);
        tries++;
    } while (body_hits(x, y, 0) && tries < 100);
    s.food = pack(x, y);
}

static void load_best(void)
{
    char buf[16];
    s.best = 0;
    memset(buf, 0, sizeof(buf));
    if (FileOpen(1)) {
        FileReadBuffer(buf, (uint16_t)sizeof(buf));
        FileClose();
        if (buf[0] == 'B' && buf[1] == '=') {
            s.best = (uint16_t)stock_parse_u32(&buf[2]);
        }
    }
}

static void save_best(void)
{
    char buf[16];
    char n[6];
    if (!s.dirty_best) {
        return;
    }
    stock_u32_to_str(s.best, n, sizeof(n));
    buf[0] = 'B';
    buf[1] = '=';
    buf[2] = '\0';
    strncat(buf, n, sizeof(buf) - strlen(buf) - 1);
    if (FileOpen(1)) {
        FileWriteBuffer(buf, (uint16_t)sizeof(buf));
        FileClose();
        s.dirty_best = 0;
    }
}

static void die(void)
{
    s.mode = MODE_DEAD;
    s.paused = 0;
    if (s.score > s.best) {
        s.best = s.score;
        s.dirty_best = 1;
        save_best();
    }
}

static void start_game(void)
{
    s.len = 3;
    s.body[0] = pack(6, 2);
    s.body[1] = pack(5, 2);
    s.body[2] = pack(4, 2);
    s.dx = 1;
    s.dy = 0;
    s.pending_dx = 1;
    s.pending_dy = 0;
    s.score = 0;
    s.mode = MODE_RUN;
    s.paused = 0;
    s.ate_flash = 0;
    s.last_tick = GetUptimeCentiseconds();
    place_food();
}

static void draw(void)
{
    char line[GRID_W + 3];
    char hdr[40];
    char a[6];
    char b[6];
    uint8_t y;
    uint8_t x;
    uint8_t i;
    uint8_t left = (uint8_t)((40 - (GRID_W + 2)) / 2 + 1);

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();

    stock_u32_to_str(s.score, a, sizeof(a));
    stock_u32_to_str(s.best, b, sizeof(b));
    if (s.mode == MODE_IDLE) {
        PutStringCentered(1, "S N A K E");
        PutStringCentered(3, "Chase *  grow  don't crash");
        if (s.best) {
            strncpy(hdr, "Best ", sizeof(hdr) - 1);
            hdr[sizeof(hdr) - 1] = '\0';
            strncat(hdr, b, sizeof(hdr) - strlen(hdr) - 1);
            PutStringCentered(5, hdr);
        } else {
            PutStringCentered(5, "No best yet — go!");
        }
        PutStringCentered(8, "Enter=play  Arrows=steer");
        return;
    }

    strncpy(hdr, "Score ", sizeof(hdr) - 1);
    hdr[sizeof(hdr) - 1] = '\0';
    strncat(hdr, a, sizeof(hdr) - strlen(hdr) - 1);
    strncat(hdr, "   Best ", sizeof(hdr) - strlen(hdr) - 1);
    strncat(hdr, b, sizeof(hdr) - strlen(hdr) - 1);
    PutStringCentered(1, hdr);

    for (y = 0; y < GRID_H; y++) {
        line[0] = '|';
        for (x = 0; x < GRID_W; x++) {
            line[x + 1] = ' ';
        }
        line[GRID_W + 1] = '|';
        line[GRID_W + 2] = '\0';

        if (unpack_y(s.food) == y) {
            x = unpack_x(s.food);
            if (x < GRID_W) {
                line[x + 1] = '*';
            }
        }
        for (i = 0; i < s.len; i++) {
            if (unpack_y(s.body[i]) == y) {
                x = unpack_x(s.body[i]);
                if (x < GRID_W) {
                    line[x + 1] = (i == 0) ? 'O' : 'o';
                }
            }
        }
        SetCursor((uint8_t)(y + 2), left, CURSOR_MODE_HIDE);
        PutStringRaw(line);
    }

    if (s.mode == MODE_DEAD) {
        if (s.score >= s.best && s.score > 0) {
            PutStringCentered(8, "New best! Enter=again");
        } else {
            PutStringCentered(8, "Crash! Enter=again");
        }
    } else if (s.paused) {
        PutStringCentered(8, "Paused — Space=go");
    } else if (s.ate_flash) {
        PutStringCentered(8, "Nom!  Space=pause");
        s.ate_flash = 0;
    } else {
        PutStringCentered(8, "Arrows  Space=pause");
    }
}

static void step(void)
{
    uint8_t hx;
    uint8_t hy;
    uint8_t nx;
    uint8_t ny;
    uint8_t i;
    uint8_t grow = 0;

    if (s.mode != MODE_RUN || s.paused) {
        return;
    }

    if (!(s.pending_dx == (int8_t)(-s.dx) && s.pending_dy == (int8_t)(-s.dy))) {
        s.dx = s.pending_dx;
        s.dy = s.pending_dy;
    }

    hx = unpack_x(s.body[0]);
    hy = unpack_y(s.body[0]);
    nx = (uint8_t)(hx + s.dx);
    ny = (uint8_t)(hy + s.dy);

    if (nx >= GRID_W || ny >= GRID_H || body_hits(nx, ny, 1)) {
        die();
        draw();
        return;
    }

    if (pack(nx, ny) == s.food) {
        grow = 1;
        s.score++;
        s.ate_flash = 1;
        if (s.len < MAX_LEN) {
            s.len++;
        }
    }

    for (i = (uint8_t)(s.len - 1); i > 0; i--) {
        s.body[i] = s.body[i - 1];
    }
    s.body[0] = pack(nx, ny);

    if (grow) {
        place_food();
    }
    s.last_tick = GetUptimeCentiseconds();
    draw();
}

static void maybe_tick(void)
{
    uint32_t now;
    if (s.mode != MODE_RUN || s.paused) {
        return;
    }
    now = GetUptimeCentiseconds();
    if ((uint32_t)(now - s.last_tick) >= tick_delay()) {
        step();
    }
}

static void set_dir(int8_t dx, int8_t dy)
{
    if (s.mode != MODE_RUN || s.paused) {
        return;
    }
    /* Same direction again = manual nudge (helps if idle is quiet). */
    if (dx == s.pending_dx && dy == s.pending_dy) {
        step();
        return;
    }
    if (!(dx == (int8_t)(-s.dx) && dy == (int8_t)(-s.dy))) {
        s.pending_dx = dx;
        s.pending_dy = dy;
    }
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.mode = MODE_IDLE;
        s.rng = 1;
        break;
    case MSG_SETFOCUS:
        load_best();
        if (s.mode == MODE_RUN) {
            s.mode = MODE_IDLE;
            s.paused = 0;
        }
        draw();
        break;
    case MSG_KILLFOCUS:
        save_best();
        break;
    case MSG_IDLE:
        maybe_tick();
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_ENTER || code == KEY_SEND) {
            start_game();
            draw();
        } else if (code == KEY_SPACE || code == KEY_SPELL_CHECK) {
            if (s.mode == MODE_RUN) {
                s.paused = s.paused ? 0 : 1;
                if (!s.paused) {
                    s.last_tick = GetUptimeCentiseconds();
                }
                draw();
            }
        } else if (code == KEY_UP) {
            set_dir(0, -1);
        } else if (code == KEY_DOWN) {
            set_dir(0, 1);
        } else if (code == KEY_LEFT) {
            set_dir(-1, 0);
        } else if (code == KEY_RIGHT) {
            set_dir(1, 0);
        } else if (code == KEY_CLEAR_FILE) {
            s.best = 0;
            s.dirty_best = 1;
            save_best();
            s.mode = MODE_IDLE;
            draw();
        } else {
            maybe_tick();
        }
        break;
    }
    default:
        break;
    }
}
