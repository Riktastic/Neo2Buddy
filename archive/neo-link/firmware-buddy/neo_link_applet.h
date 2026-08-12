/**

 * @file neo_link_applet.h

 * @brief Install and verify the bundled NeoLinkChat.OS3KApp on a connected Neo.

 */



#pragma once



#include "esp_err.h"

#include "neo_applet.h"

#include <stdbool.h>

#include <stddef.h>

#include <stdint.h>



/** Pointer/length of the firmware-embedded NeoLinkChat.OS3KApp blob. */

const uint8_t *neo_link_applet_blob(size_t *out_len);



/** Parse the embedded .OS3KApp header (no USB). */

esp_err_t neo_link_applet_bundled_info(neo_applet_info_t *out);



/**

 * True when @p installed matches the firmware bundle (version + sane ramUsage).

 * @p installed must be a single applet entry (id 0xA1C0).

 */

bool neo_link_applet_installed_is_current(const neo_applet_info_t *installed);



typedef struct {

    bool installed;

    bool up_to_date;

    bool sync_busy;

    uint8_t bundled_major;

    uint8_t bundled_minor;

    uint8_t bundled_rev;

    uint8_t installed_major;

    uint8_t installed_minor;

    uint8_t installed_rev;

    uint32_t bundled_ram;

    uint32_t installed_ram;

    uint32_t bundled_rom;

    uint32_t installed_rom;

    esp_err_t last_sync_err;

    char last_sync_msg[64];

} neo_link_applet_status_t;



void neo_link_applet_init(void);

void neo_link_applet_get_status(neo_link_applet_status_t *out);



/**

 * Flip to ASM, install/replace when missing or outdated, return to keyboard.

 * @param force  Replace even when version appears current.

 */

esp_err_t neo_link_applet_ensure_current(bool force);



/**

 * Install (or replace) Neo Link Chat on the connected Neo.

 * Leaves the keyboard unavailable until the user returns from ASM.

 */

esp_err_t neo_link_applet_install(bool replace_existing);



/** Optional auto-check when Neo is already in comms mode (menuconfig). Prefer `link install`. */

void neo_link_applet_on_neo_connected(void);


