/**
 * @file MathDrill.c
 * @brief Math / science drill — random problems, instant feedback, score.
 *
 * Enter = answer current problem
 * Find  = next problem (skip)
 * Tab   = cycle category (Mix / Arith / Algebra / Units)
 * Clear File = reset score
 */
#include "os3k.h"
#include "stock_math.h"
#include "stock_fmt.h"
#include <string.h>

#define APP_ID 0xA1B7
#define CAT_MIX 0
#define CAT_ARITH 1
#define CAT_ALGEBRA 2
#define CAT_UNITS 3
#define CAT_COUNT 4
#define PROMPT_LEN 40

APPLET_HEADER_BEGIN
    APPLET_ID(APP_ID)
    APPLET_NAME("Math Drill")
    APPLET_INFO("Arithmetic algebra unit drills")
    APPLET_VERSION(1, 0, "a")
    APPLET_LANGUAGE_EN_US
APPLET_HEADER_END

typedef struct {
    uint16_t rng;
    uint8_t category; /* preferred; MIX picks randomly */
    uint8_t kind;     /* last generated family */
    int32_t answer;
    uint16_t correct;
    uint16_t asked;
    uint16_t streak;
    uint16_t best_streak;
    char prompt[PROMPT_LEN];
    char feedback[28];
} drill_state_t;

static drill_state_t s;

static const char k_cat_name[CAT_COUNT][8] = {
    "Mix", "Arith", "Algebra", "Units"
};

static uint16_t rng_next(void)
{
    s.rng ^= (uint16_t)(GetUptimeMilliseconds() & 0xFFFF);
    s.rng = (uint16_t)(s.rng * 1103515245u + 12345u);
    return s.rng;
}

static uint16_t rng_range(uint16_t lo, uint16_t hi_inclusive)
{
    uint16_t span;
    if (hi_inclusive < lo) {
        return lo;
    }
    span = (uint16_t)(hi_inclusive - lo + 1);
    return (uint16_t)(lo + stock_urem16(rng_next(), span));
}

static void set_feedback(const char *msg)
{
    strncpy(s.feedback, msg ? msg : "", sizeof(s.feedback) - 1);
    s.feedback[sizeof(s.feedback) - 1] = '\0';
}

static void make_arith(void)
{
    uint8_t op = (uint8_t)rng_range(0, 3);
    uint16_t a, b;
    char na[6], nb[6];

    s.kind = CAT_ARITH;
    if (op == 0) { /* + */
        a = rng_range(2, 49);
        b = rng_range(2, 49);
        s.answer = (int32_t)a + (int32_t)b;
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, na, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " + ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, " = ?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (op == 1) { /* - */
        a = rng_range(10, 99);
        b = rng_range(1, a);
        s.answer = (int32_t)a - (int32_t)b;
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, na, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " - ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, " = ?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (op == 2) { /* * */
        a = rng_range(2, 12);
        b = rng_range(2, 12);
        s.answer = (int32_t)a * (int32_t)b;
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, na, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " x ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, " = ?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else { /* / exact */
        b = rng_range(2, 12);
        a = (uint16_t)(b * rng_range(2, 12));
        s.answer = (int32_t)stock_udiv32(a, b);
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, na, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " / ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, " = ?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    }
}

static void make_algebra(void)
{
    uint8_t form = (uint8_t)rng_range(0, 2);
    uint16_t a, b, x;
    char na[6], nb[6];

    s.kind = CAT_ALGEBRA;
    if (form == 0) { /* x + a = b */
        a = rng_range(1, 40);
        x = rng_range(1, 40);
        b = (uint16_t)(x + a);
        s.answer = (int32_t)x;
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, "x + ", sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, na, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, " = ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, "  x=?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (form == 1) { /* a + x = b */
        a = rng_range(1, 40);
        x = rng_range(1, 40);
        b = (uint16_t)(a + x);
        s.answer = (int32_t)x;
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, na, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " + x = ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, "  x=?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else { /* a * x = b */
        a = rng_range(2, 9);
        x = rng_range(2, 12);
        b = (uint16_t)(a * x);
        s.answer = (int32_t)x;
        stock_u32_to_str(a, na, sizeof(na));
        stock_u32_to_str(b, nb, sizeof(nb));
        strncpy(s.prompt, na, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " x = ", sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, nb, sizeof(s.prompt) - strlen(s.prompt) - 1);
        strncat(s.prompt, "  x=?", sizeof(s.prompt) - strlen(s.prompt) - 1);
    }
}

static void make_units(void)
{
    uint8_t u = (uint8_t)rng_range(0, 5);
    uint16_t v;
    char nv[8];

    s.kind = CAT_UNITS;
    if (u == 0) { /* cm -> mm */
        v = rng_range(1, 50);
        s.answer = (int32_t)v * 10;
        stock_u32_to_str(v, nv, sizeof(nv));
        strncpy(s.prompt, nv, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " cm = ? mm", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (u == 1) { /* m -> cm */
        v = rng_range(1, 20);
        s.answer = (int32_t)v * 100;
        stock_u32_to_str(v, nv, sizeof(nv));
        strncpy(s.prompt, nv, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " m = ? cm", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (u == 2) { /* ft -> in */
        v = rng_range(1, 12);
        s.answer = (int32_t)v * 12;
        stock_u32_to_str(v, nv, sizeof(nv));
        strncpy(s.prompt, nv, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " ft = ? in", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (u == 3) { /* min -> sec */
        v = rng_range(1, 20);
        s.answer = (int32_t)v * 60;
        stock_u32_to_str(v, nv, sizeof(nv));
        strncpy(s.prompt, nv, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " min = ? s", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else if (u == 4) { /* kg -> g */
        v = rng_range(1, 9);
        s.answer = (int32_t)v * 1000;
        stock_u32_to_str(v, nv, sizeof(nv));
        strncpy(s.prompt, nv, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " kg = ? g", sizeof(s.prompt) - strlen(s.prompt) - 1);
    } else { /* hours -> min */
        v = rng_range(1, 8);
        s.answer = (int32_t)v * 60;
        stock_u32_to_str(v, nv, sizeof(nv));
        strncpy(s.prompt, nv, sizeof(s.prompt) - 1);
        s.prompt[sizeof(s.prompt) - 1] = '\0';
        strncat(s.prompt, " h = ? min", sizeof(s.prompt) - strlen(s.prompt) - 1);
    }
}

static void next_problem(void)
{
    uint8_t cat = s.category;
    if (cat == CAT_MIX) {
        cat = (uint8_t)rng_range(CAT_ARITH, CAT_UNITS);
    }
    if (cat == CAT_ARITH) {
        make_arith();
    } else if (cat == CAT_ALGEBRA) {
        make_algebra();
    } else {
        make_units();
    }
}

static void draw_screen(void)
{
    char line[41];
    char a[8], b[8], c[8];

    SetCursorMode(CURSOR_MODE_HIDE);
    ClearScreen();
    PutStringCentered(1, "Math Drill");

    stock_u32_to_str(s.correct, a, sizeof(a));
    stock_u32_to_str(s.asked, b, sizeof(b));
    stock_u32_to_str(s.streak, c, sizeof(c));
    strncpy(line, a, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, "/", sizeof(line) - strlen(line) - 1);
    strncat(line, b, sizeof(line) - strlen(line) - 1);
    strncat(line, "  streak ", sizeof(line) - strlen(line) - 1);
    strncat(line, c, sizeof(line) - strlen(line) - 1);
    if (s.best_streak > 1) {
        strncat(line, " (best ", sizeof(line) - strlen(line) - 1);
        stock_u32_to_str(s.best_streak, a, sizeof(a));
        strncat(line, a, sizeof(line) - strlen(line) - 1);
        strncat(line, ")", sizeof(line) - strlen(line) - 1);
    }
    PutStringCentered(2, line);

    ClearRowCols(3, 1, 40);
    strncpy(line, "[", sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';
    strncat(line, k_cat_name[s.category], sizeof(line) - strlen(line) - 1);
    strncat(line, "]  ", sizeof(line) - strlen(line) - 1);
    strncat(line, k_cat_name[s.kind], sizeof(line) - strlen(line) - 1);
    PutStringCentered(3, line);

    ClearRowCols(5, 1, 40);
    PutStringCentered(5, s.prompt);

    ClearRowCols(7, 1, 40);
    if (s.feedback[0]) {
        PutStringCentered(7, s.feedback);
    }

    PutStringCentered(8, "Enter=ans Tab=cat Find=skip");
}

static int32_t parse_signed(const char *str)
{
    int32_t n = 0;
    int neg = 0;
    if (!str) {
        return 0;
    }
    while (*str == ' ') {
        str++;
    }
    if (*str == '-') {
        neg = 1;
        str++;
    } else if (*str == '+') {
        str++;
    }
    while (*str >= '0' && *str <= '9') {
        n = n * 10 + (*str - '0');
        str++;
    }
    return neg ? -n : n;
}

static void ask_answer(void)
{
    static const Key_e exit_keys[] = { KEY_ESC, KEY_ENTER, KEY_NONE };
    char buf[12];
    uint8_t len = 0;
    KeyMod_e key;
    int32_t got;
    char right[12];

    buf[0] = '\0';
    ClearRowCols(8, 1, 40);
    SetCursor(8, 1, CURSOR_MODE_SHOW);
    PutStringRaw("Answer: ");
    key = (KeyMod_e)TextBox(buf, &len, 10, exit_keys, false);
    SetCursorMode(CURSOR_MODE_HIDE);
    if ((key & 0xFF) == KEY_ESC || len == 0) {
        draw_screen();
        return;
    }
    got = parse_signed(buf);
    s.asked++;
    if (got == s.answer) {
        s.correct++;
        s.streak++;
        if (s.streak > s.best_streak) {
            s.best_streak = s.streak;
        }
        set_feedback("Correct!");
        if (s.streak >= 5 && (s.streak % 5) == 0) {
            set_feedback("On fire!");
        }
    } else {
        s.streak = 0;
        if (s.answer < 0) {
            right[0] = '-';
            stock_u32_to_str((uint32_t)(-s.answer), right + 1, sizeof(right) - 1);
        } else {
            stock_u32_to_str((uint32_t)s.answer, right, sizeof(right));
        }
        strncpy(s.feedback, "No — ", sizeof(s.feedback) - 1);
        s.feedback[sizeof(s.feedback) - 1] = '\0';
        strncat(s.feedback, right, sizeof(s.feedback) - strlen(s.feedback) - 1);
    }
    next_problem();
    draw_screen();
}

void ProcessMessage(Message_e message, uint32_t param, uint32_t *status)
{
    *status = 0;
    switch (message) {
    case MSG_INIT:
        memset(&s, 0, sizeof(s));
        s.rng = (uint16_t)GetUptimeMilliseconds();
        s.category = CAT_MIX;
        next_problem();
        break;
    case MSG_SETFOCUS:
        if (!s.prompt[0]) {
            next_problem();
        }
        draw_screen();
        break;
    case MSG_KEY: {
        uint8_t code = (uint8_t)((KeyMod_e)param & 0xFF);
        if (code == KEY_ENTER || code == KEY_SEND) {
            ask_answer();
        } else if (code == KEY_FIND || code == KEY_RIGHT) {
            set_feedback("Skipped");
            next_problem();
            draw_screen();
        } else if (code == KEY_TAB) {
            s.category++;
            if (s.category >= CAT_COUNT) {
                s.category = 0;
            }
            set_feedback("");
            next_problem();
            draw_screen();
        } else if (code == KEY_CLEAR_FILE) {
            s.correct = 0;
            s.asked = 0;
            s.streak = 0;
            set_feedback("Score reset");
            next_problem();
            draw_screen();
        }
        break;
    }
    default:
        break;
    }
}
