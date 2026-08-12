/**
 * @file stock_fmt.h
 * @brief Tiny shared format helpers (no libc printf dependency).
 */
#pragma once
#include "stock_math.h"
#include <stddef.h>
#include <string.h>

static inline void stock_u32_to_str(uint32_t n, char *out, size_t out_sz)
{
    char tmp[12];
    char *p = tmp + sizeof(tmp);
    *--p = '\0';
    if (n == 0) {
        *--p = '0';
    } else {
        while (n && p > tmp) {
            uint32_t q = stock_udiv32(n, 10);
            *--p = (char)('0' + (char)(n - q * 10));
            n = q;
        }
    }
    strncpy(out, p, out_sz - 1);
    out[out_sz - 1] = '\0';
}

static inline uint32_t stock_parse_u32(const char *str)
{
    uint32_t n = 0;
    if (!str) {
        return 0;
    }
    while (*str >= '0' && *str <= '9') {
        n = n * 10u + (uint32_t)(*str - '0');
        str++;
    }
    return n;
}
