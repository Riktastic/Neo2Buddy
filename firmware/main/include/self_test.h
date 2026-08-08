/**
 * @file self_test.h
 * @brief Boot-time smoke tests (dev builds / CI).
 *
 * Runs quick checks on auth, Neo message framing, filename sanitization, and
 * other core helpers. Returns failure count; main may log warnings without
 * blocking normal operation. Gated by CONFIG_BUDDY_SELF_TEST where applicable.
 */

#pragma once

#include <stdint.h>

/** Run all registered smoke tests; returns number of failures. */
uint32_t self_test_run(void);
