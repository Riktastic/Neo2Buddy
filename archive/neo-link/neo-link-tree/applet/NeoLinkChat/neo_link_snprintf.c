/**
 * Minimal snprintf for freestanding m68k (no newlib).
 *
 * NOT linked into Neo Link Chat (see build-docker.ps1). Kept for experiments only.
 * Linking this + neo_link_protocol.c blows applet ramUsage past NEO_LINK_APPLET_RAM_BUDGET.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>

static int sn_putc(char *str, size_t size, size_t *pos, char c)
{
    if (*pos + 1 < size) {
        str[*pos] = c;
    }
    (*pos)++;
    return 0;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
    va_list ap;
    size_t pos = 0;
    const char *p;

    if (!fmt) {
        return 0;
    }
    va_start(ap, fmt);
    for (p = fmt; *p; p++) {
        if (*p != '%') {
            sn_putc(str, size, &pos, *p);
            continue;
        }
        p++;
        if (*p == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s) {
                s = "(null)";
            }
            while (*s) {
                sn_putc(str, size, &pos, *s++);
            }
        } else if (*p == '0' && *(p + 1) == '2' && *(p + 2) == 'x') {
            unsigned int v = va_arg(ap, unsigned int) & 0xFFu;
            static const char hex[] = "0123456789abcdef";
            sn_putc(str, size, &pos, hex[(v >> 4) & 0xF]);
            sn_putc(str, size, &pos, hex[v & 0xF]);
            p += 2;
        } else if (*p == '%') {
            sn_putc(str, size, &pos, '%');
        } else {
            sn_putc(str, size, &pos, '%');
            if (*p) {
                sn_putc(str, size, &pos, *p);
            }
        }
    }
    va_end(ap);
    if (size > 0) {
        str[(pos < size) ? pos : (size - 1)] = '\0';
    }
    return (int)pos;
}
