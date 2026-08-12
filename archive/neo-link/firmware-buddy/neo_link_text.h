/**
 * @file neo_link_text.h
 * @brief Plain-text encode for Neo Link mailbox files (NOT AlphaWord).
 *
 * Limits: neo-link/protocol/neo_link_limits.h (included below).
 * OS3K rules: neo-link/docs/os3k-applet.md
 */

#pragma once

#include <stddef.h>

#include "neo_link_limits.h"

/**
 * Copy @p utf8 into @p out as Neo-safe printable ASCII.
 * - Strips CR; LF/TAB → space
 * - Non-ASCII UTF-8 → '?'
 * - Truncates to @p out_cap - 1 and NUL-terminates
 * Returns length excluding NUL.
 */
size_t neo_link_text_to_mailbox(const char *utf8, char *out, size_t out_cap);

/** Light markdown/noise cleanup for LLM replies before mailbox encode. */
void neo_link_text_strip_markup(char *s);
