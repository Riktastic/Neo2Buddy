/**
 * @file stock_math.h
 * @brief Soft urem/udiv — freestanding m68k has no libgcc __modsi3.
 */
#pragma once
#include <stdint.h>

static inline uint16_t stock_urem16(uint16_t a, uint16_t b)
{
    if (b == 0) {
        return 0;
    }
    while (a >= b) {
        a = (uint16_t)(a - b);
    }
    return a;
}

static inline uint32_t stock_udiv32(uint32_t n, uint32_t d)
{
    uint32_t q = 0;
    if (d == 0) {
        return 0;
    }
    while (n >= d) {
        uint32_t t = d;
        uint32_t qbit = 1;
        while ((t << 1) > t && (t << 1) <= n) {
            t <<= 1;
            qbit <<= 1;
        }
        n -= t;
        q += qbit;
    }
    return q;
}
