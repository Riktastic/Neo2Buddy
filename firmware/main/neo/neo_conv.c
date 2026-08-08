/**
 * @file neo_conv.c
 * @brief AlphaWord charset to/from UTF-8 (NeoTools text_file.py parity).
 *
 * The Neo does not store UTF-8. Each byte 0x00-0xFF maps to a Unicode code point
 * via tables embedded at build time (character_map text files). Export walks Neo bytes
 * and emits UTF-8; import walks UTF-8 code points and emits Neo bytes.
 *
 * Three maps match NeoTools: en-us (default), ua-pc, ua-mac. Wrong map produces
 * mojibake but does not corrupt the device — conversion happens only on the host.
 */

#include "neo_conv.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

extern const char _binary_en_us_txt_start[];
extern const char _binary_en_us_txt_end[];
extern const char _binary_ua_pc_txt_start[];
extern const char _binary_ua_pc_txt_end[];
extern const char _binary_ua_mac_txt_start[];
extern const char _binary_ua_mac_txt_end[];

#define NEO_CHARMAP_LINE_MAX 16
#define NEO_CHARMAP_LINES 256

typedef struct {
    char neo_to_unicode[NEO_CHARMAP_LINES][NEO_CHARMAP_LINE_MAX];
    uint8_t unicode_to_neo[NEO_CHARMAP_LINES][NEO_CHARMAP_LINE_MAX];
    uint8_t unicode_to_neo_len[NEO_CHARMAP_LINES];
    bool loaded;
} neo_charmap_t;

static neo_charmap_t s_maps[3];

static esp_err_t neo_charmap_load_blob(neo_charmap_t *map, const char *start, const char *end)
{
    if (!map || !start || !end || end <= start) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(map, 0, sizeof(*map));
    size_t line = 0;
    size_t col = 0;
    for (const char *p = start; p < end && line < NEO_CHARMAP_LINES; ++p) {
        if (*p == '\r') {
            continue;
        }
        if (*p == '\n') {
            map->neo_to_unicode[line][col] = '\0';
            if (col > 0) {
                memcpy(map->unicode_to_neo[line], map->neo_to_unicode[line], col + 1);
                map->unicode_to_neo_len[line] = (uint8_t)col;
            }
            line++;
            col = 0;
            continue;
        }
        if (col + 1 >= NEO_CHARMAP_LINE_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        map->neo_to_unicode[line][col++] = *p;
    }
    if (line != NEO_CHARMAP_LINES) {
        return ESP_ERR_INVALID_SIZE;
    }
    map->loaded = true;
    return ESP_OK;
}

neo_charmap_id_t neo_charmap_by_name(const char *name)
{
    if (!name || name[0] == '\0' || strcmp(name, "default") == 0 || strcmp(name, "en") == 0 ||
        strcmp(name, "en-us") == 0) {
        return NEO_CHARMAP_EN_US;
    }
    if (strcmp(name, "ua-pc") == 0) {
        return NEO_CHARMAP_UA_PC;
    }
    if (strcmp(name, "ua-mac") == 0) {
        return NEO_CHARMAP_UA_MAC;
    }
    return NEO_CHARMAP_EN_US;
}

esp_err_t neo_charmap_init(void)
{
    esp_err_t err = neo_charmap_load_blob(&s_maps[NEO_CHARMAP_EN_US], _binary_en_us_txt_start, _binary_en_us_txt_end);
    if (err != ESP_OK) {
        return err;
    }
    err = neo_charmap_load_blob(&s_maps[NEO_CHARMAP_UA_PC], _binary_ua_pc_txt_start, _binary_ua_pc_txt_end);
    if (err != ESP_OK) {
        return err;
    }
    return neo_charmap_load_blob(&s_maps[NEO_CHARMAP_UA_MAC], _binary_ua_mac_txt_start, _binary_ua_mac_txt_end);
}

static neo_charmap_t *neo_charmap_get(neo_charmap_id_t map)
{
    if (map < 0 || map >= 3) {
        map = NEO_CHARMAP_EN_US;
    }
    if (!s_maps[map].loaded && neo_charmap_init() != ESP_OK) {
        return NULL;
    }
    return &s_maps[map];
}

static uint8_t neo_lookup_unicode(const neo_charmap_t *map, const char *utf8, size_t len)
{
    for (size_t i = 0; i < NEO_CHARMAP_LINES; ++i) {
        if (map->unicode_to_neo_len[i] == len && memcmp(map->unicode_to_neo[i], utf8, len) == 0) {
            return (uint8_t)i;
        }
    }
    return 0;
}

static size_t utf8_next(const char *text, size_t *index, char out[NEO_CHARMAP_LINE_MAX])
{
    const unsigned char c0 = (unsigned char)text[*index];
    if (c0 == '\0') {
        return 0;
    }
    size_t len = 1;
    if ((c0 & 0x80) == 0) {
        len = 1;
    } else if ((c0 & 0xE0) == 0xC0) {
        len = 2;
    } else if ((c0 & 0xF0) == 0xE0) {
        len = 3;
    } else if ((c0 & 0xF8) == 0xF0) {
        len = 4;
    }
    if (len >= NEO_CHARMAP_LINE_MAX) {
        len = NEO_CHARMAP_LINE_MAX - 1;
    }
    memcpy(out, text + *index, len);
    out[len] = '\0';
    *index += len;
    return len;
}

size_t neo_conv_export_buf_size(size_t neo_len)
{
    /* en-us is ~1:1 after padding strip; allow 2× for multi-byte map entries. */
    if (neo_len > (SIZE_MAX / 2) - 32) {
        return SIZE_MAX;
    }
    return neo_len * 2 + 32;
}

/**
 * Export Neo raw bytes to UTF-8 for backup files.
 *
 * Mirrors NeoTools text_file.py export rules:
 *   - Skip Neo control bytes (0xa4, 0xa7, 0x8f, 0xa1..0xbf range)
 *   - Map 0x0d → newline, 0x81/0xa1 → space, 0x8d/0xa3 → tab
 *   - 0xb0 introduces an escaped second byte (smart quotes etc.)
 *   - Unknown bytes become '?' so we never emit invalid UTF-8
 *
 * This runs on the Buddy after READ_RAW_FILE — the Neo always stores AlphaWord
 * encoding; conversion is host-side only.
 */
size_t neo_conv_export_text_from_neo(const uint8_t *neo, size_t neo_len, char *out, size_t out_len,
                                     neo_charmap_id_t map_id)
{
    if (!neo || !out || out_len == 0) {
        return 0;
    }
    neo_charmap_t *map = neo_charmap_get(map_id);
    if (!map) {
        return 0;
    }

    size_t written = 0;
    for (size_t index = 0; index < neo_len && written + 1 < out_len;) {
        uint8_t code = neo[index++];
        bool is_escaped = false;

        if (code == 0xa4 || code == 0xa7) {
            continue;
        }
        if (code == 0x0d) {
            code = 0x0a;
        } else if (code == 0x81 || code == 0xa1) {
            code = 0x20;
        } else if (code == 0x8d || code == 0xa3) {
            code = 0x09;
        } else if (code == 0x8f) {
            continue;
        } else if (code == 0xad) {
            code = 0x2d;
        } else if (code == 0xb0) {
            if (index + 1 > neo_len) {
                break;
            }
            is_escaped = true;
            code = neo[index++];
            if (index < neo_len && neo[index] == 0xb0) {
                index++;
            }
        } else if (code >= 0xa1 && code <= 0xbf) {
            continue;
        }

        const char *mapped = NULL;
        if ((code == 0x09 || code == 0x0a || code == 0x0d) && !is_escaped) {
            if (written + 1 >= out_len) {
                break;
            }
            out[written++] = (char)code;
            continue;
        }
        mapped = map->neo_to_unicode[code];
        if (!mapped || mapped[0] == '\0') {
            if (written + 1 >= out_len) {
                break;
            }
            out[written++] = '?';
            continue;
        }
        size_t mlen = strlen(mapped);
        if (written + mlen >= out_len) {
            break;
        }
        memcpy(out + written, mapped, mlen);
        written += mlen;
    }
    out[written] = '\0';
    return written;
}

/**
 * Import UTF-8 text into Neo bytes before WRITE_RAW_FILE.
 *
 * Walks UTF-8 code points, looks up each in the embedded charmap table, and
 * emits the corresponding Neo byte (0x00..0xFF). Unmapped characters become
 * '?' (byte 0x3f). Line endings normalize to Neo 0x0d.
 */
esp_err_t neo_conv_import_text_to_neo(const char *text, neo_charmap_id_t map_id, uint8_t *out, size_t out_capacity,
                                      size_t *out_length)
{
    if (!text || !out || !out_length || out_capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    neo_charmap_t *map = neo_charmap_get(map_id);
    if (!map) {
        return ESP_FAIL;
    }

    const size_t min_file_size = 256;
    size_t out_pos = 0;
    size_t softbreak_count = 0;
    size_t hardbreak_count = 0;
    size_t last_break_opportunity = 0;

    for (size_t index = 0; text[index] != '\0';) {
        char utf8[NEO_CHARMAP_LINE_MAX];
        size_t ulen = utf8_next(text, &index, utf8);
        if (ulen == 0) {
            break;
        }

        bool escape = false;
        uint8_t code = neo_lookup_unicode(map, utf8, ulen);
        if (utf8[0] == 0xac && ulen == 1 && code == 0) {
            code = 0xac;
        }
        if (code == 0x81) {
            code = 0xac;
        }
        if ((code >= 0xa1 && code <= 0xbf) || code == 0x09 || code == 0x0a || code == 0x0d) {
            escape = true;
        }
        if (ulen == 1 && utf8[0] == '\t') {
            code = 0x09;
            escape = true;
        } else if (ulen == 1 && (utf8[0] == '\r' || utf8[0] == '\n')) {
            code = 0x0d;
            escape = true;
        }

        bool is_break = !escape && code == 0x0d;
        bool is_breakable = !escape && (code == 0x2d || code == 0x20 || code == 0x09);
        hardbreak_count++;
        softbreak_count++;

        if (is_break) {
            last_break_opportunity = 0;
            softbreak_count = 0;
            hardbreak_count = 0;
        } else if (is_breakable) {
            last_break_opportunity = out_pos;
            hardbreak_count = 0;
        } else if (hardbreak_count >= 24) {
            if (out_pos + 1 > out_capacity) {
                return ESP_ERR_NO_MEM;
            }
            out[out_pos++] = 0x8f;
            softbreak_count = 0;
            hardbreak_count = 0;
            last_break_opportunity = 0;
        }

        if (escape) {
            if (out_pos + 3 > out_capacity) {
                return ESP_ERR_NO_MEM;
            }
            out[out_pos++] = 0xb0;
            out[out_pos++] = code;
            out[out_pos++] = 0xb0;
        } else {
            if (out_pos + 1 > out_capacity) {
                return ESP_ERR_NO_MEM;
            }
            out[out_pos++] = code;
        }

        if (softbreak_count >= 40 && last_break_opportunity > 0) {
            uint8_t last = out[last_break_opportunity];
            if (last == 0x2d) {
                out[last_break_opportunity] = 0xad;
            } else if (last == 0x20) {
                out[last_break_opportunity] = 0x81;
            } else if (last == 0x09) {
                out[last_break_opportunity] = 0x8d;
            }
            softbreak_count = 0;
            hardbreak_count = 0;
            last_break_opportunity = 0;
        }
    }

    if (out_pos < min_file_size) {
        while (out_pos < min_file_size) {
            if (out_pos >= out_capacity) {
                return ESP_ERR_NO_MEM;
            }
            out[out_pos++] = 0xa7;
        }
    }

    *out_length = out_pos;
    return ESP_OK;
}
