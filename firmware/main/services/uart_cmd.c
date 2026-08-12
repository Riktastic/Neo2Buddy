/**
 * @file uart_cmd.c
 * @brief Password-protected UART command console (NeoTools-style test surface).
 *
 * Linux-style interface: help, help COMMAND, COMMAND --help, Tab completion,
 * and command history via esp_console/linenoise.
 */

#include "uart_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "auth.h"
#include "board_config.h"
#include "ble_hid.h"
#include "cJSON.h"
#include "cloud_sync.h"
#include "device_status.h"
#include "esp_chip_info.h"
#include "esp_console.h"
#include "linenoise/linenoise.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#if HAVE_WIFI_WEB
#include "esp_wifi.h"
#endif
#include "file_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_debug.h"
#include "log_buffer.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "neo_applet.h"
#include "neo_conv.h"
#include "neo_debug.h"
#include "neo_file.h"
#include "neo_import.h"
#include "neo_autobackup.h"
#include "neo_live.h"
#include "neo_space.h"
#include "settings.h"
#include "usb_host_neo.h"
#include "wifi_manager.h"
#include "factory_reset.h"
#if HAVE_OLED
#include "display.h"
#endif
#if HAVE_SDCARD
#include "sd_card.h"
#include "sd_format.h"
#endif
#if CONFIG_BUDDY_NEO_LINK
#include "neo_link_llm.h"
#include "neo_link_applet.h"
#include "neo_link_limits.h"
#include "usb_host_neo.h"
#endif

static const char *TAG = "uart_cmd";

#define UART_SESSION_SECONDS 600
#define UART_REPL_STACK_SIZE 16384
#define UART_JSON_ALLOC_DEFAULT 4096
#define UART_JSON_ALLOC_MAX 16384

static bool s_authed = false;
static uint64_t s_auth_expires = 0;

/* -------------------------------------------------------------------------- */
/* Help metadata                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *command;
    const char *summary;
    const char *usage;
    const char *description;
    const char *examples;
    bool auth_required;
    const char *hint;
    esp_console_cmd_func_t func;
} uart_cmd_entry_t;

static void uart_print_command_help(const uart_cmd_entry_t *entry);
static void uart_print_topic_help(const char *topic);
static const uart_cmd_entry_t *uart_find_command(const char *name);

static bool uart_session_valid(void)
{
    if (!s_authed) {
        return false;
    }
    uint64_t now = (uint64_t)esp_timer_get_time() / 1000000ULL;
    if (s_auth_expires != 0 && now > s_auth_expires) {
        s_authed = false;
        return false;
    }
    s_auth_expires = now + UART_SESSION_SECONDS;
    return true;
}

static void uart_touch_session(void)
{
    if (s_authed) {
        s_auth_expires = ((uint64_t)esp_timer_get_time() / 1000000ULL) + UART_SESSION_SECONDS;
    }
}

static bool uart_require_auth(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (uart_session_valid()) {
        return true;
    }
    printf("login: authentication required\n");
    printf("Try 'help login' or 'login --help'\n");
    return false;
}

static bool uart_wants_help(int argc, char **argv)
{
    /* Top-level: argv[0] is the command name; flags are in argv[1..].
     * Subcommands (after group dispatch): argv[0] may be -h/--help. */
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return true;
        }
    }
    return false;
}

/** Remove --debug/-d from argv in-place. Returns true if a debug flag was present. */
static bool uart_strip_debug_flag(int *argc, char **argv)
{
    if (!argc || !argv || *argc <= 0) {
        return false;
    }
    bool found = false;
    int write = 0;
    for (int read = 0; read < *argc; ++read) {
        if (argv[read] != NULL &&
            (strcmp(argv[read], "--debug") == 0 || strcmp(argv[read], "-d") == 0)) {
            found = true;
            continue;
        }
        argv[write++] = argv[read];
    }
    *argc = write;
    return found;
}

/** Remove --json from argv. Returns true if present (machine-readable output). */
static bool uart_strip_json_flag(int *argc, char **argv)
{
    if (!argc || !argv || *argc <= 0) {
        return false;
    }
    bool found = false;
    int write = 0;
    for (int read = 0; read < *argc; ++read) {
        if (argv[read] != NULL && strcmp(argv[read], "--json") == 0) {
            found = true;
            continue;
        }
        argv[write++] = argv[read];
    }
    *argc = write;
    return found;
}

static bool s_cli_json = false;

static bool uart_want_json(void)
{
    return s_cli_json;
}

/** Join argv[start..] with spaces into @p out (NUL-terminated). */
static void uart_join_argv(int argc, char **argv, int start, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!argv || start < 0) {
        return;
    }
    for (int i = start; i < argc; ++i) {
        if (argv[i] == NULL || argv[i][0] == '\0') {
            continue;
        }
        if (out[0] != '\0') {
            strlcat(out, " ", out_size);
        }
        strlcat(out, argv[i], out_size);
    }
}

/**
 * Parse SSID (+ optional password) from argv after "connect".
 *
 * ESP-IDF's console already handles double quotes and strips them, so
 *   device wifi connect "My SSID" "secret"
 * arrives as argv = ["My SSID", "secret"]. Re-joining those and splitting on
 * spaces would break the SSID again — use argv as-is in that case.
 *
 * Single quotes are NOT special to ESP-IDF, so
 *   device wifi connect 'My SSID' 'secret'
 * arrives broken; re-join and parse with both ' and " support.
 * Empty password: device wifi connect "MySSID" ""
 */
static bool uart_parse_wifi_credentials(int argc, char **argv, int start,
                                        char *ssid, size_t ssid_size,
                                        char *password, size_t password_size)
{
    if (!ssid || ssid_size == 0 || !password || password_size == 0) {
        return false;
    }
    ssid[0] = '\0';
    password[0] = '\0';
    if (!argv || start < 0 || start >= argc || argv[start] == NULL) {
        return false;
    }

    const char *first = argv[start];
    const bool looks_single_quoted = (first[0] == '\'');
    const bool looks_raw_double = (first[0] == '"');

    /* Path A: tokens already cleaned by esp_console (double quotes / no spaces). */
    if (!looks_single_quoted && !looks_raw_double) {
        if (strlcpy(ssid, first, ssid_size) >= ssid_size || ssid[0] == '\0') {
            return false;
        }
        if (start + 1 >= argc) {
            return true; /* open network / no password arg */
        }
        /* Preserve empty "" password (esp_console yields an empty argv entry). */
        if (argv[start + 1] == NULL) {
            return true;
        }
        if (start + 2 == argc) {
            strlcpy(password, argv[start + 1], password_size);
            return true;
        }
        /* Multiple password tokens (unusual) — join with spaces. */
        uart_join_argv(argc, argv, start + 1, password, password_size);
        return true;
    }

    /* Path B: single-quoted or quote chars still present — reassemble and parse. */
    char line[192];
    line[0] = '\0';
    for (int i = start; i < argc; ++i) {
        if (argv[i] == NULL) {
            continue;
        }
        if (line[0] != '\0') {
            strlcat(line, " ", sizeof(line));
        }
        /* Keep empty tokens as "" so an empty password survives the join. */
        if (argv[i][0] == '\0') {
            strlcat(line, "\"\"", sizeof(line));
        } else {
            strlcat(line, argv[i], sizeof(line));
        }
    }
    if (line[0] == '\0') {
        return false;
    }

    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0') {
        return false;
    }

    char quote = 0;
    if (*p == '"' || *p == '\'') {
        quote = *p++;
    }

    size_t n = 0;
    while (*p != '\0') {
        if (quote) {
            if (*p == quote) {
                ++p;
                break;
            }
        } else if (*p == ' ' || *p == '\t') {
            break;
        }
        if (n + 1 >= ssid_size) {
            return false;
        }
        ssid[n++] = *p++;
    }
    ssid[n] = '\0';
    if (ssid[0] == '\0') {
        return false;
    }

    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0') {
        return true;
    }

    quote = 0;
    if (*p == '"' || *p == '\'') {
        quote = *p++;
    }
    n = 0;
    while (*p != '\0') {
        if (quote) {
            if (*p == quote) {
                break;
            }
        } else if (*p == ' ' || *p == '\t') {
            while (*p != '\0') {
                if (n + 1 >= password_size) {
                    return false;
                }
                password[n++] = *p++;
            }
            break;
        }
        if (n + 1 >= password_size) {
            return false;
        }
        password[n++] = *p++;
    }
    password[n] = '\0';
    return true;
}

static bool uart_parse_bool(const char *text, bool *out)
{
    if (!text || !out) {
        return false;
    }
    if (strcasecmp(text, "on") == 0 || strcasecmp(text, "true") == 0 || strcasecmp(text, "yes") == 0 ||
        strcmp(text, "1") == 0) {
        *out = true;
        return true;
    }
    if (strcasecmp(text, "off") == 0 || strcasecmp(text, "false") == 0 || strcasecmp(text, "no") == 0 ||
        strcmp(text, "0") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool uart_help_requested(int argc, char **argv, const char *command)
{
    if (uart_wants_help(argc, argv)) {
        const uart_cmd_entry_t *entry = uart_find_command(command);
        if (entry) {
            uart_print_command_help(entry);
        } else {
            printf("No detailed help for '%s'\n", command);
        }
        return true;
    }
    return false;
}

static void uart_print_command_help(const uart_cmd_entry_t *entry)
{
    if (!entry) {
        return;
    }
    printf("NAME\n    %s - %s\n\n", entry->command, entry->summary);
    printf("SYNOPSIS\n    %s\n\n", entry->usage);
    if (entry->description && entry->description[0]) {
        printf("DESCRIPTION\n    %s\n\n", entry->description);
    }
    printf("AUTHENTICATION\n    %s\n\n", entry->auth_required ? "Required (login first)" : "Not required");
    if (entry->examples && entry->examples[0]) {
        printf("EXAMPLES\n%s\n", entry->examples);
    }
}

static void uart_print_usage(const char *command)
{
    const uart_cmd_entry_t *entry = uart_find_command(command);
    if (entry) {
        printf("Usage: %s\n", entry->usage);
        printf("Try '%s --help' or 'help %s' for more information.\n", entry->command, entry->command);
    }
}

static bool parse_u16(const char *text, uint16_t *out)
{
    if (!text || !out || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || value > 0xffffUL) {
        return false;
    }
    *out = (uint16_t)value;
    return true;
}

static bool parse_u8(const char *text, uint8_t *out)
{
    if (!text || !out || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || value > 255UL) {
        return false;
    }
    *out = (uint8_t)value;
    return true;
}

static esp_err_t uart_neo_require(void)
{
    esp_err_t err = usb_host_neo_ensure_comms();
    if (err != ESP_OK) {
        neo_usb_scan_result_t scan = {0};
        if (usb_host_neo_rescan(&scan) == ESP_OK) {
            err = usb_host_neo_ensure_comms();
        }
    }
    if (err != ESP_OK) {
        printf("neo: %s (try 'neo rescan' or replug the Neo USB cable)\n", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

/** Print and free a heap JSON string; avoids large stack buffers in the REPL task. */
static int uart_print_json_text(char *json, const char *fail_msg)
{
    if (!json) {
        printf("%s: out of memory\n", fail_msg);
        return 1;
    }
    printf("%s\n", json);
    free(json);
    return 0;
}

/** Serialize a cJSON tree to the console, then delete the tree. */
static int uart_print_cjson(cJSON *json, const char *fail_msg)
{
    if (!json) {
        printf("%s: out of memory\n", fail_msg);
        return 1;
    }
    char *out = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return uart_print_json_text(out, fail_msg);
}

static size_t uart_clamp_json_alloc(size_t cap)
{
    if (cap == 0) {
        return UART_JSON_ALLOC_DEFAULT;
    }
    if (cap > UART_JSON_ALLOC_MAX) {
        return UART_JSON_ALLOC_MAX;
    }
    return cap;
}

/* Forward declarations for command handlers. */
static int cmd_help(int argc, char **argv);
static int cmd_login(int argc, char **argv);
static int cmd_logout(int argc, char **argv);
static int cmd_status(int argc, char **argv);
static int cmd_reboot(int argc, char **argv);
static int cmd_neo(int argc, char **argv);
static int cmd_neo_info(int argc, char **argv);
static int cmd_neo_mode(int argc, char **argv);
static int cmd_neo_version(int argc, char **argv);
static int cmd_neo_rescan(int argc, char **argv);
static int cmd_neo_restart(int argc, char **argv);
static int cmd_neo_list_applets(int argc, char **argv);
static int cmd_neo_fetch_applet(int argc, char **argv);
static void uart_print_json_escaped(const char *s);
static int cmd_neo_list_files(int argc, char **argv);
static int cmd_neo_file_attrs(int argc, char **argv);
static int cmd_neo_read_file(int argc, char **argv);
static int cmd_neo_read_all(int argc, char **argv);
static int cmd_neo_backup(int argc, char **argv);
static int cmd_neo_autobackup(int argc, char **argv);
static int cmd_neo_clear_file(int argc, char **argv);
static int cmd_neo_write_file(int argc, char **argv);
static int cmd_neo_space(int argc, char **argv);
static int cmd_neo_space_used(int argc, char **argv);
static int cmd_neo_debug(int argc, char **argv);
static int cmd_neo_remove_applet(int argc, char **argv);
static int cmd_neo_remove_all(int argc, char **argv);
static int cmd_device(int argc, char **argv);
static int cmd_device_info(int argc, char **argv);
static int cmd_device_wifi(int argc, char **argv);
static int cmd_device_storage(int argc, char **argv);
static int cmd_device_sd_remount(int argc, char **argv);
static int cmd_device_sd_format(int argc, char **argv);
static int cmd_device_logs(int argc, char **argv);
static int cmd_device_settings(int argc, char **argv);
static int cmd_device_ble(int argc, char **argv);
static int cmd_device_battery(int argc, char **argv);
static int cmd_device_heap(int argc, char **argv);
static int cmd_device_name(int argc, char **argv);
static int cmd_device_password(int argc, char **argv);
static int cmd_device_sync(int argc, char **argv);
static int cmd_device_wifi_recovery(int argc, char **argv);
static int cmd_device_reboot(int argc, char **argv);
static int cmd_device_factory_reset(int argc, char **argv);
static int cmd_files(int argc, char **argv);
static int cmd_files_list(int argc, char **argv);
static int cmd_files_probe(int argc, char **argv);
static int cmd_files_view(int argc, char **argv);
static int cmd_files_delete(int argc, char **argv);
static int cmd_files_rename(int argc, char **argv);
static int cmd_keyboard(int argc, char **argv);
static int cmd_keyboard_recent(int argc, char **argv);
static int cmd_keyboard_clear(int argc, char **argv);
static int cmd_keyboard_raw(int argc, char **argv);
static int cmd_keyboard_keylog(int argc, char **argv);
#if CONFIG_BUDDY_NEO_LINK
static int cmd_link(int argc, char **argv);
static int cmd_link_llm(int argc, char **argv);
static int cmd_link_install(int argc, char **argv);
static int cmd_link_verify(int argc, char **argv);
#endif
static int cmd_ping(int argc, char **argv);

static const char *device_reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:
        return "task-watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep-sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    default:
        return "unknown";
    }
}

static const char *device_chip_model_str(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    switch (chip.model) {
    case CHIP_ESP32:
        return "classic";
    case CHIP_ESP32S2:
        return "S2";
    case CHIP_ESP32S3:
        return "S3";
    case CHIP_ESP32C3:
        return "C3";
    case CHIP_ESP32C2:
        return "C2";
    case CHIP_ESP32C6:
        return "C6";
    case CHIP_ESP32H2:
        return "H2";
    case CHIP_ESP32P4:
        return "P4";
    default:
        return "unknown";
    }
}

static const char *device_wifi_state_str(device_wifi_state_t state)
{
    switch (state) {
    case DEVICE_WIFI_UNCONFIGURED:
        return "unconfigured";
    case DEVICE_WIFI_CONNECTING:
        return "connecting";
    case DEVICE_WIFI_CONNECTED:
        return "connected";
    case DEVICE_WIFI_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

static const uart_cmd_entry_t s_commands[] = {
    {
        .command = "help",
        .summary = "display help for commands",
        .usage = "help [COMMAND]",
        .description = "Without arguments, lists all commands by category. "
                       "With COMMAND, shows a manual page for that command (supports prefixes, e.g. 'help neo').",
        .examples = "    help\n    help login\n    help neo list-files\n",
        .auth_required = false,
        .hint = "[COMMAND]",
        .func = cmd_help,
    },
    {
        .command = "login",
        .summary = "authenticate for protected commands",
        .usage = "login PASSWORD",
        .description = "Opens a session (10 minutes, extended on use). Uses the same password as the web portal.",
        .examples = "    login neo2buddy\n",
        .auth_required = false,
        .hint = "PASSWORD",
        .func = cmd_login,
    },
    {
        .command = "logout",
        .summary = "end authenticated session",
        .usage = "logout",
        .description = "Clears the UART session without affecting web portal tokens.",
        .examples = "    logout\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_logout,
    },
    {
        .command = "status",
        .summary = "device and USB summary",
        .usage = "status",
        .description = "Prints JSON with Wi-Fi state, Neo USB connection, mode, and whether you are logged in.",
        .examples = "    status\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_status,
    },
    {
        .command = "ping",
        .summary = "check internet reachability",
        .usage = "ping [HOST]",
        .description = "Resolves HOST (default one.one.one.one) and opens a TCP connection to port 443. "
                       "Reports DNS and connect latency. Requires an active home Wi-Fi association.",
        .examples = "    ping\n    ping dns.google\n    ping 1.1.1.1\n",
        .auth_required = false,
        .hint = "[HOST]",
        .func = cmd_ping,
    },
    {
        .command = "reboot",
        .summary = "restart the ESP32",
        .usage = "reboot",
        .description = "Software reset of the companion firmware.",
        .examples = "    reboot\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_reboot,
    },
    {
        .command = "neo",
        .summary = "Neo USB command group",
        .usage = "neo",
        .description = "Lists all neo subcommands. Each subcommand also accepts -h and --help.",
        .examples = "    neo\n    help neo\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_neo,
    },
    {
        .command = "neo info",
        .summary = "Neo system information",
        .usage = "neo info",
        .description = "Version string, USB mode, and available ROM/RAM on the connected Neo.",
        .examples = "    neo info\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_info,
    },
    {
        .command = "neo mode",
        .summary = "USB operating mode",
        .usage = "neo mode",
        .description = "Reports keyboard, comms, or unknown mode plus connection state.",
        .examples = "    neo mode\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_mode,
    },
    {
        .command = "neo version",
        .summary = "Neo firmware version",
        .usage = "neo version",
        .description = "Reads the VERSION command from the connected Neo.",
        .examples = "    neo version\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_version,
    },
    {
        .command = "neo rescan",
        .summary = "scan USB and connect Neo",
        .usage = "neo rescan",
        .description = "Scans OTG1 and starts HID-to-comms flip if a Neo is found.",
        .examples = "    neo rescan\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_rescan,
    },
    {
        .command = "neo restart",
        .summary = "restart Neo into keyboard mode",
        .usage = "neo restart",
        .description = "Sends RESTART to the Neo (NeoTools flip-to-keyboard).",
        .examples = "    neo restart\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_restart,
    },
    {
        .command = "neo list-applets",
        .summary = "list installed SmartApplets",
        .usage = "neo list-applets",
        .description = "Returns JSON array of applet id, name, sizes, and file counts.",
        .examples = "    neo list-applets\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_list_applets,
    },
    {
        .command = "neo fetch-applet",
        .summary = "download a SmartApplet package (NeoTools applets fetch)",
        .usage = "neo fetch-applet APPLET_ID [FILENAME]",
        .description = "Fetches the on-device ROM package via REQUEST_READ_APPLET (same as NeoTools "
                       "`applets fetch`). Writes a .os3kapp (or SystemRom.os3kos for id 0) under "
                       "buddy storage. Validates C0FFEEAD/CAFEFEED for regular applets.",
        .examples = "    neo fetch-applet 0xA007\n    neo fetch-applet 40967 ControlPanel.os3kapp\n    neo fetch-applet 0\n",
        .auth_required = true,
        .hint = "APPLET_ID [FILENAME]",
        .func = cmd_neo_fetch_applet,
    },
    {
        .command = "neo list-files",
        .summary = "list document files on an applet",
        .usage = "neo list-files [APPLET_ID]",
        .description = "Lists files with name, space, and allocated size. Default applet is AlphaWord (0xA000).",
        .examples = "    neo list-files\n    neo list-files 0xA000\n",
        .auth_required = true,
        .hint = "[APPLET_ID]",
        .func = cmd_neo_list_files,
    },
    {
        .command = "neo file-attrs",
        .summary = "file metadata by index",
        .usage = "neo file-attrs APPLET_ID FILE_INDEX",
        .description = "Returns JSON with name, file space, and allocation size.",
        .examples = "    neo file-attrs 0xA000 1\n",
        .auth_required = true,
        .hint = "APPLET_ID FILE_INDEX",
        .func = cmd_neo_file_attrs,
    },
    {
        .command = "neo read-file",
        .summary = "read a file as text",
        .usage = "neo read-file FILE_INDEX [MAX_CHARS]\nneo read-file APPLET_ID FILE_INDEX [MAX_CHARS]",
        .description =
            "Reads one Neo file and prints the full converted UTF-8 text (AlphaWord default).\n"
            "  FILE_INDEX  AlphaWord file number (usually 1-8).\n"
            "  MAX_CHARS   optional truncate (omit for the whole story).\n"
            "  APPLET_ID   optional; defaults to AlphaWord 0xA000.\n"
            "Raw Neo bytes are freed before printing to keep peak RAM lower.",
        .examples = "    neo read-file 1\n    neo read-file 1 256\n    neo read-file 0xA000 1\n",
        .auth_required = true,
        .hint = "FILE_INDEX [MAX] | APPLET_ID FILE_INDEX [MAX]",
        .func = cmd_neo_read_file,
    },
    {
        .command = "neo read-all",
        .summary = "backup all non-empty files",
        .usage = "neo read-all [APPLET_ID]",
        .description = "Same as 'neo backup' with no file index: save every non-empty file to SD/spiflash.",
        .examples = "    neo read-all\n    neo read-all 0xA000\n",
        .auth_required = true,
        .hint = "[APPLET_ID]",
        .func = cmd_neo_read_all,
    },
    {
        .command = "neo backup",
        .summary = "backup one file or all files to SD/spiflash",
        .usage = "neo backup [FILE_INDEX]\nneo backup APPLET_ID [FILE_INDEX]",
        .description =
            "Saves Neo file(s) as UTF-8 text under /sdcard/neo or /spiflash/neo.\n"
            "  (no args)           backup all AlphaWord files (1-8)\n"
            "  FILE_INDEX          backup one AlphaWord file (1-8)\n"
            "  APPLET_ID           backup all files on that applet\n"
            "  APPLET_ID FILE_INDEX  backup one file on that applet",
        .examples = "    neo backup\n    neo backup 1\n    neo backup 0xA000\n    neo backup 0xA000 3\n",
        .auth_required = true,
        .hint = "[FILE_INDEX] | APPLET_ID [FILE_INDEX]",
        .func = cmd_neo_backup,
    },
    {
        .command = "neo autobackup",
        .summary = "auto-backup on connect (on/off/now/status)",
        .usage = "neo autobackup [on|off|now|status]",
        .description =
            "When enabled, plugging in Neo (keyboard mode) flips to manager mode,\n"
            "backs up changed AlphaWord files to SD/spiflash, then returns Neo to\n"
            "keyboard emulation via restart.\n"
            "  (no args)|status  show whether auto-backup is enabled\n"
            "  on                enable auto-backup on connect\n"
            "  off               disable\n"
            "  now               run once now, then return to keyboard",
        .examples = "    neo autobackup\n    neo autobackup on\n    neo autobackup now\n",
        .auth_required = true,
        .hint = "[on|off|now|status]",
        .func = cmd_neo_autobackup,
    },
    {
        .command = "neo clear-file",
        .summary = "clear a file by name or space",
        .usage = "neo clear-file APPLET_ID NAME_OR_SPACE",
        .description = "Clears file contents. NAME_OR_SPACE is a filename or slot number 1-8.",
        .examples = "    neo clear-file 0xA000 MyDoc\n    neo clear-file 0xA000 3\n",
        .auth_required = true,
        .hint = "APPLET_ID NAME_OR_SPACE",
        .func = cmd_neo_clear_file,
    },
    {
        .command = "neo write-file",
        .summary = "write UTF-8 text into a Neo file slot",
        .usage = "neo write-file APPLET_ID FILE_INDEX TEXT...",
        .description = "Converts UTF-8 to Neo charset and writes the file (interrupts keyboard mode). "
                       "Limited by the UART line length (~500 characters of text).",
        .examples = "    neo write-file 0xA000 1 Hello from UART\n",
        .auth_required = true,
        .hint = "APPLET FILE TEXT",
        .func = cmd_neo_write_file,
    },
    {
        .command = "neo space",
        .summary = "available ROM and RAM",
        .usage = "neo space",
        .description = "Device-wide free storage from GET_AVAIL_SPACE.",
        .examples = "    neo space\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_space,
    },
    {
        .command = "neo space-used",
        .summary = "RAM used by an applet",
        .usage = "neo space-used APPLET_ID",
        .description = "Per-applet RAM usage and file count.",
        .examples = "    neo space-used 0xA000\n",
        .auth_required = true,
        .hint = "APPLET_ID",
        .func = cmd_neo_space_used,
    },
    {
        .command = "neo debug",
        .summary = "Neo protocol trace / verbose logging",
        .usage = "neo debug [on|off|status|LIMIT]",
        .description =
            "Protocol traces stay in a ring by default (quiet serial).\n"
            "  on|off|status   sticky verbose logging to serial/portal logs\n"
            "  LIMIT           dump recent ring events as JSON (default 20)\n"
            "Any neo command also accepts --debug for one-shot verbose output.",
        .examples = "    neo debug on\n    neo debug\n    neo backup --debug\n",
        .auth_required = true,
        .hint = "[on|off|status|LIMIT]",
        .func = cmd_neo_debug,
    },
    {
        .command = "neo remove-applet",
        .summary = "remove one SmartApplet",
        .usage = "neo remove-applet APPLET_ID",
        .description = "Permanently removes the applet and its files from the Neo.",
        .examples = "    neo remove-applet 0x1234\n",
        .auth_required = true,
        .hint = "APPLET_ID",
        .func = cmd_neo_remove_applet,
    },
    {
        .command = "neo remove-all",
        .summary = "remove all SmartApplets",
        .usage = "neo remove-all",
        .description = "Removes every installed applet. The Neo reboots afterward.",
        .examples = "    neo remove-all\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_neo_remove_all,
    },
    {
        .command = "device",
        .summary = "device command group",
        .usage = "device",
        .description = "Lists device management commands (Wi-Fi, storage, logs, settings). "
                       "These operate on the companion device, not the connected Neo.",
        .examples = "    device\n    help device\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_device,
    },
    {
        .command = "device info",
        .summary = "firmware and runtime summary",
        .usage = "device info",
        .description = "Chip model, IDF version, uptime, reset reason, and heap summary.",
        .examples = "    device info\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_info,
    },
    {
        .command = "device wifi",
        .summary = "Wi-Fi status, scan, connect, or mode",
        .usage = "device wifi [status|scan|connect SSID [PASSWORD]|mode home|direct|ap]",
        .description = "Without arguments, shows association state. scan lists nearby APs; "
                       "connect saves credentials and joins home Wi-Fi (quote SSIDs with spaces: "
                       "\"My SSID\" or 'My SSID'); mode home|direct switches portal networking; "
                       "ap starts the device hotspot.",
        .examples = "    device wifi\n"
                     "    device wifi scan\n"
                     "    device wifi connect MyHome secret\n"
                     "    device wifi connect \"My wifi network\" \"secret with spaces\"\n"
                     "    device wifi connect GuestWifi \"\"\n"
                     "    device wifi mode direct\n",
        .auth_required = true,
        .hint = "[SUBCOMMAND]",
        .func = cmd_device_wifi,
    },
    {
        .command = "device wifi-recovery",
        .summary = "start recovery hotspot",
        .usage = "device wifi-recovery",
        .description = "Forces the recovery AP when home Wi-Fi is unavailable.",
        .examples = "    device wifi-recovery\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_wifi_recovery,
    },
    {
        .command = "device storage",
        .summary = "internal flash and SD usage",
        .usage = "device storage",
        .description = "SPIFFS portal partition usage and SD card space when mounted.",
        .examples = "    device storage\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_storage,
    },
    {
        .command = "device sd-remount",
        .summary = "try to mount the SD card",
        .usage = "device sd-remount",
        .description = "Re-attempts SD card detection and mount.",
        .examples = "    device sd-remount\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_sd_remount,
    },
    {
        .command = "device sd-format",
        .summary = "format the microSD card (destructive)",
        .usage = "device sd-format",
        .description = "Starts an async FAT format. All SD files are erased. Poll device storage for progress.",
        .examples = "    device sd-format\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_sd_format,
    },
    {
        .command = "device logs",
        .summary = "recent device log lines",
        .usage = "device logs [LIMIT]",
        .description = "Returns JSON array from the in-memory log ring (default 30 lines).",
        .examples = "    device logs\n    device logs 50\n",
        .auth_required = true,
        .hint = "[LIMIT]",
        .func = cmd_device_logs,
    },
    {
        .command = "device settings",
        .summary = "show or update persisted settings",
        .usage = "device settings [set KEY VALUE...]",
        .description = "Without arguments, prints settings (passwords redacted). "
                       "set updates one field: device_name, keyboard_layout, display_brightness, "
                       "sleep_timeout_seconds, require_portal_auth, auto_backup_on_connect, "
                       "auto_cloud_sync_after_backup, neo_label, network_mode, hotspot_ssid, "
                       "hotspot_password, wifi_ssid, wifi_password, wifi_dhcp, wifi_ip, "
                       "wifi_netmask, wifi_gateway, wifi_dns.",
        .examples = "    device settings\n"
                     "    device settings set display_brightness 80\n"
                     "    device settings set neo_label Classroom-A\n"
                     "    device settings set require_portal_auth on\n",
        .auth_required = true,
        .hint = "[set KEY VALUE]",
        .func = cmd_device_settings,
    },
    {
        .command = "device password",
        .summary = "change the portal / UART password",
        .usage = "device password CURRENT NEW",
        .description = "Verifies CURRENT then stores NEW (8..63 characters). Shared by web and UART.",
        .examples = "    device password neo2buddy MyNewPass1\n",
        .auth_required = true,
        .hint = "CURRENT NEW",
        .func = cmd_device_password,
    },
    {
        .command = "device sync",
        .summary = "cloud backup sync config and run",
        .usage = "device sync [status|config|set KEY VALUE|test|run]",
        .description = "Configure WebDAV/S3 upload of local backups. set keys: provider, enabled, "
                       "endpoint, folder, bucket, region, username, secret.",
        .examples = "    device sync\n"
                     "    device sync set provider webdav\n"
                     "    device sync set endpoint https://example.com/dav\n"
                     "    device sync set enabled on\n"
                     "    device sync test\n"
                     "    device sync run\n",
        .auth_required = true,
        .hint = "[SUBCOMMAND]",
        .func = cmd_device_sync,
    },
    {
        .command = "device ble",
        .summary = "Bluetooth keyboard bridge status and control",
        .usage = "device ble [status|bonds|pair on|off|clear|preview TEXT|send|cancel]",
        .description = "Neo keys passthrough to the paired BLE host when connected. Bonds persist "
                       "across reboot; bonds lists saved hosts; pair on opens a 2-minute window for "
                       "a new host; clear forgets all bonded hosts. preview/send still types portal "
                       "text to the host; cancel clears the queue.",
        .examples = "    device ble\n"
                     "    device ble bonds\n"
                     "    device ble pair on\n"
                     "    device ble clear\n"
                     "    device ble preview \"Hello from UART\"\n"
                     "    device ble send\n"
                     "    device ble cancel\n",
        .auth_required = true,
        .hint = "[SUBCOMMAND]",
        .func = cmd_device_ble,
    },
    {
        .command = "device battery",
        .summary = "battery voltage and charge state",
        .usage = "device battery",
        .description = "Reports millivolts, percent, and charging flag when battery support is compiled in.",
        .examples = "    device battery\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_battery,
    },
    {
        .command = "device heap",
        .summary = "free memory statistics",
        .usage = "device heap",
        .description = "Current free heap, historical minimum, and largest free block.",
        .examples = "    device heap\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_heap,
    },
    {
        .command = "device name",
        .summary = "get or set device name",
        .usage = "device name [NEW_NAME]",
        .description = "Without arguments, prints the current name. With NEW_NAME, saves to NVS.",
        .examples = "    device name\n    device name \"My Neo2\"\n",
        .auth_required = true,
        .hint = "[NEW_NAME]",
        .func = cmd_device_name,
    },
    {
        .command = "device reboot",
        .summary = "restart the companion firmware",
        .usage = "device reboot",
        .description = "Software reset of the companion device.",
        .examples = "    device reboot\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_reboot,
    },
    {
        .command = "device factory-reset",
        .summary = "erase settings and internal backups",
        .usage = "device factory-reset PASSWORD",
        .description = "Restore firmware defaults: wipe NVS settings, cloud sync, portal password, "
                       "and internal backup files. SD card files are kept. Re-enter the portal password "
                       "to confirm, then the buddy reboots.",
        .examples = "    device factory-reset neo2buddy\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_device_factory_reset,
    },
    {
        .command = "files",
        .summary = "buddy-local backup file group",
        .usage = "files",
        .description = "List, view, delete, rename, or probe backup files on SD/SPIFFS storage.",
        .examples = "    files\n    files probe\n    help files\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_files,
    },
    {
        .command = "files list",
        .summary = "list local backup files",
        .usage = "files list",
        .description = "JSON array of name, size, and modified time for buddy-stored backups.",
        .examples = "    files list\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_files_list,
    },
    {
        .command = "files probe",
        .summary = "verify backup storage read/write",
        .usage = "files probe",
        .description = "Writes a small probe file, lists it, reads it back, then deletes it. "
                       "Use this to confirm SPIFFS/SD backup storage before relying on autobackup.",
        .examples = "    files probe\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_files_probe,
    },
    {
        .command = "files view",
        .summary = "print the start of a backup file",
        .usage = "files view NAME",
        .description = "Reads up to 4 KiB of a local backup and prints it to the console.",
        .examples = "    files view MyBackup.txt\n",
        .auth_required = true,
        .hint = "NAME",
        .func = cmd_files_view,
    },
    {
        .command = "files delete",
        .summary = "delete a local backup file",
        .usage = "files delete NAME",
        .description = "Removes one file from buddy storage.",
        .examples = "    files delete MyBackup.txt\n",
        .auth_required = true,
        .hint = "NAME",
        .func = cmd_files_delete,
    },
    {
        .command = "files rename",
        .summary = "rename a local backup file",
        .usage = "files rename OLD NEW",
        .description = "Renames within the storage directory.",
        .examples = "    files rename old.txt new.txt\n",
        .auth_required = true,
        .hint = "OLD NEW",
        .func = cmd_files_rename,
    },
    {
        .command = "keyboard",
        .summary = "live Neo typing buffer group",
        .usage = "keyboard",
        .description = "Live keyboard monitor: recent text, clear buffer, raw HID, or keylog toggle.",
        .examples = "    keyboard\n    help keyboard\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_keyboard,
    },
    {
        .command = "keyboard recent",
        .summary = "show live typing buffer",
        .usage = "keyboard recent",
        .description = "Snapshot of text typed on the Neo while in USB keyboard mode.",
        .examples = "    keyboard recent\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_keyboard_recent,
    },
    {
        .command = "keyboard clear",
        .summary = "clear the live typing buffer",
        .usage = "keyboard clear",
        .description = "Empties the portal live-typing buffer.",
        .examples = "    keyboard clear\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_keyboard_clear,
    },
    {
        .command = "keyboard raw",
        .summary = "recent raw HID keyboard reports",
        .usage = "keyboard raw",
        .description = "JSON of recent Neo HID interrupt reports for layout diagnostics.",
        .examples = "    keyboard raw\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_keyboard_raw,
    },
    {
        .command = "keyboard keylog",
        .summary = "enable/disable per-keystroke UART logging",
        .usage = "keyboard keylog [on|off|status]",
        .description = "Mirrors each decoded Neo keystroke to the serial console. Off by default "
                       "because it is expensive while typing; use only for field debugging.",
        .examples = "    keyboard keylog\n    keyboard keylog on\n    keyboard keylog off\n",
        .auth_required = true,
        .hint = "[on|off|status]",
        .func = cmd_keyboard_keylog,
    },
#if CONFIG_BUDDY_NEO_LINK
    {
        .command = "link",
        .summary = "Neo Link (LLM proxy) command group",
        .usage = "link",
        .description = "Lists Neo Link subcommands.",
        .examples = "    link\n    help link\n",
        .auth_required = false,
        .hint = NULL,
        .func = cmd_link,
    },
    {
        .command = "link llm",
        .summary = "configure or test the Neo Link LLM proxy",
        .usage = "link llm [status|set KEY VALUE|test [PROMPT]|clear-context]",
        .description = "NVS namespace neo_link (same as portal /api/v1/link/llm). set keys: enabled, "
                       "base_url, api_key, model, system, max_tokens, max_rpm, context_turns. "
                       "api_key is never printed; use set api_key \"\" to clear.",
        .examples = "    link llm\n"
                     "    link llm set enabled on\n"
                     "    link llm set base_url https://api.openai.com/v1\n"
                     "    link llm set api_key sk-...\n"
                     "    link llm set model gpt-4o-mini\n"
                     "    link llm test\n"
                     "    link llm clear-context\n",
        .auth_required = true,
        .hint = "[SUBCOMMAND]",
        .func = cmd_link_llm,
    },
    {
        .command = "link install",
        .summary = "install bundled BetaWise HelloWorld onto the Neo",
        .usage = "link install [--no-replace]",
        .description = "Pushes the firmware-embedded BetaWise HelloWorld.OS3KApp (id 0xA1A0) over USB. "
                       "Requires a connected Neo. Replaces an existing 0xA1A0 install by default.",
        .examples = "    link install\n    link install --no-replace\n",
        .auth_required = true,
        .hint = "[--no-replace]",
        .func = cmd_link_install,
    },
    {
        .command = "link verify",
        .summary = "fetch installed applet from Neo and compare to bundled blob",
        .usage = "link verify",
        .description = "Reads applet 0xA1A0 from the Neo over USB and compares the first "
                       "bundled-size bytes to the firmware-embedded .OS3KApp.",
        .examples = "    link verify\n",
        .auth_required = true,
        .hint = NULL,
        .func = cmd_link_verify,
    },
#endif
};

static const size_t s_command_count = sizeof(s_commands) / sizeof(s_commands[0]);

static const uart_cmd_entry_t *uart_find_command(const char *name)
{
    if (!name || name[0] == '\0') {
        return NULL;
    }
    for (size_t i = 0; i < s_command_count; ++i) {
        if (strcmp(s_commands[i].command, name) == 0) {
            return &s_commands[i];
        }
    }
    return NULL;
}

/** Match longest registered subcommand under @p group (e.g. "neo", "device"). */
static const uart_cmd_entry_t *uart_find_subcommand(const char *group, int argc, char **argv, int *consumed)
{
    if (!group || !consumed || argc < 1 || argv == NULL) {
        return NULL;
    }
    char full[80];
    for (int tokens = argc; tokens >= 1; --tokens) {
        int pos = snprintf(full, sizeof(full), "%s", group);
        for (int i = 0; i < tokens && pos < (int)sizeof(full) - 1; ++i) {
            if (argv[i] == NULL) {
                continue;
            }
            pos += snprintf(full + pos, sizeof(full) - (size_t)pos, " %s", argv[i]);
        }
        const uart_cmd_entry_t *entry = uart_find_command(full);
        if (entry && entry->func != NULL) {
            *consumed = tokens;
            return entry;
        }
    }
    return NULL;
}

static int uart_dispatch_group(const char *group, int argc, char **argv)
{
    bool one_shot_debug = uart_strip_debug_flag(&argc, argv);
    bool want_json = uart_strip_json_flag(&argc, argv);
    bool prev_verbose = neo_debug_is_verbose();
    bool prev_json = s_cli_json;
    if (one_shot_debug) {
        neo_debug_set_verbose(true);
    }
    s_cli_json = want_json;

    int rc = 0;
    if (uart_wants_help(argc, argv)) {
        if (argc >= 1) {
            int consumed = 0;
            const uart_cmd_entry_t *entry = uart_find_subcommand(group, argc, argv, &consumed);
            if (entry) {
                uart_print_command_help(entry);
                rc = 0;
                goto done;
            }
        }
        const uart_cmd_entry_t *group_entry = uart_find_command(group);
        if (group_entry) {
            uart_print_command_help(group_entry);
        }
        rc = 0;
        goto done;
    }
    if (argc == 0) {
        uart_print_topic_help(group);
        rc = 0;
        goto done;
    }
    int consumed = 0;
    const uart_cmd_entry_t *entry = uart_find_subcommand(group, argc, argv, &consumed);
    if (entry) {
        if (entry->auth_required && !uart_session_valid()) {
            printf("login: authentication required\n");
            printf("Try 'help login' or 'login --help'\n");
            rc = 1;
            goto done;
        }
        if (entry->auth_required) {
            uart_touch_session();
        }
        rc = entry->func(argc - consumed, argv + consumed);
        goto done;
    }
    printf("%s: unknown subcommand '%s'\n", group, (argv[0] != NULL) ? argv[0] : "(null)");
    printf("Try '%s' or 'help %s'\n", group, group);
    rc = 1;

done:
    if (one_shot_debug) {
        neo_debug_set_verbose(prev_verbose);
    }
    s_cli_json = prev_json;
    return rc;
}

static void uart_print_overview_help(void)
{
    printf("Neo2 Buddy serial console\n\n");
    printf("Usage: help [COMMAND]\n");
    printf("       COMMAND [--help|-h] [--debug|-d] [--json]\n");
    printf("       Tab completes command names; Up/Down recalls history\n\n");

    printf("Session:\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strchr(e->command, ' ') != NULL || strcmp(e->command, "neo") == 0 ||
            strcmp(e->command, "device") == 0 || strcmp(e->command, "files") == 0 ||
            strcmp(e->command, "keyboard") == 0 || strcmp(e->command, "link") == 0) {
            continue;
        }
        printf("  %-18s %s\n", e->command, e->summary);
    }

    printf("\nNeo USB (login required):\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strncmp(e->command, "neo ", 4) != 0) {
            continue;
        }
        printf("  %-22s %s\n", e->command, e->summary);
    }

    printf("\nDevice (login required):\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strncmp(e->command, "device ", 7) != 0) {
            continue;
        }
        printf("  %-22s %s\n", e->command, e->summary);
    }

    printf("\nFiles (login required):\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strncmp(e->command, "files ", 6) != 0) {
            continue;
        }
        printf("  %-22s %s\n", e->command, e->summary);
    }

    printf("\nKeyboard (login required):\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strncmp(e->command, "keyboard ", 9) != 0) {
            continue;
        }
        printf("  %-22s %s\n", e->command, e->summary);
    }

#if CONFIG_BUDDY_NEO_LINK
    printf("\nNeo Link (login required):\n");
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strncmp(e->command, "link ", 5) != 0) {
            continue;
        }
        printf("  %-22s %s\n", e->command, e->summary);
    }
#endif

#if CONFIG_BUDDY_NEO_LINK
    printf("\nType 'help COMMAND' for a manual page. Use 'neo', 'device', 'files', 'keyboard', or 'link'.\n");
#else
    printf("\nType 'help COMMAND' for a manual page. Use 'neo', 'device', 'files', or 'keyboard'.\n");
#endif
}

static void uart_print_topic_help(const char *topic)
{
    const uart_cmd_entry_t *exact = uart_find_command(topic);
    if (exact) {
        uart_print_command_help(exact);
        return;
    }

    size_t topic_len = strlen(topic);
    bool any = false;

    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *e = &s_commands[i];
        if (strncmp(e->command, topic, topic_len) == 0 &&
            (e->command[topic_len] == '\0' || e->command[topic_len] == ' ')) {
            if (!any) {
                printf("Commands matching '%s':\n\n", topic);
                any = true;
            }
            printf("  %-22s %s\n", e->command, e->summary);
        }
    }

    if (any) {
        printf("\nRun 'help COMMAND' for full details on one command.\n");
        return;
    }

    printf("help: no command matching '%s'\n", topic);
    printf("Try 'help' for a list of commands.\n");
}

static int cmd_help(int argc, char **argv)
{
    if (uart_wants_help(argc, argv)) {
        uart_print_command_help(uart_find_command("help"));
        return 0;
    }
    /* esp_console: argv[0] is the command name; bare "help" has argc == 1. */
    if (argc <= 1) {
        uart_print_overview_help();
        return 0;
    }
    char topic[80];
    topic[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        if (argv[i] == NULL) {
            continue;
        }
        if (i > 1) {
            strlcat(topic, " ", sizeof(topic));
        }
        strlcat(topic, argv[i], sizeof(topic));
    }
    uart_print_topic_help(topic);
    return 0;
}

static int cmd_neo(int argc, char **argv)
{
    int sub_argc = argc > 1 ? argc - 1 : 0;
    char **sub_argv = argc > 1 ? argv + 1 : argv;
    return uart_dispatch_group("neo", sub_argc, sub_argv);
}

static int cmd_device(int argc, char **argv)
{
    int sub_argc = argc > 1 ? argc - 1 : 0;
    char **sub_argv = argc > 1 ? argv + 1 : argv;
    return uart_dispatch_group("device", sub_argc, sub_argv);
}

static int cmd_files(int argc, char **argv)
{
    int sub_argc = argc > 1 ? argc - 1 : 0;
    char **sub_argv = argc > 1 ? argv + 1 : argv;
    return uart_dispatch_group("files", sub_argc, sub_argv);
}

static int cmd_keyboard(int argc, char **argv)
{
    int sub_argc = argc > 1 ? argc - 1 : 0;
    char **sub_argv = argc > 1 ? argv + 1 : argv;
    return uart_dispatch_group("keyboard", sub_argc, sub_argv);
}

#if CONFIG_BUDDY_NEO_LINK
static int cmd_link(int argc, char **argv)
{
    int sub_argc = argc > 1 ? argc - 1 : 0;
    char **sub_argv = argc > 1 ? argv + 1 : argv;
    return uart_dispatch_group("link", sub_argc, sub_argv);
}
#endif

static int cmd_login(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "login")) {
        return 0;
    }
    if (argc < 2 || argv[1] == NULL) {
        uart_print_usage("login");
        return 1;
    }
    if (auth_login_rate_limited()) {
        printf("login: too many attempts — wait 60s\n");
        return 1;
    }
    if (!auth_check_password(argv[1])) {
        printf("login: invalid password\n");
        return 1;
    }
    s_authed = true;
    uart_touch_session();
    printf("Login successful (session %ds)\n", UART_SESSION_SECONDS);
    return 0;
}

static int cmd_logout(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "logout")) {
        return 0;
    }
    (void)argc;
    (void)argv;
    s_authed = false;
    s_auth_expires = 0;
    printf("Logged out\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "status")) {
        return 0;
    }
    (void)argc;
    (void)argv;
    device_status_t status = {0};
    device_status_get(&status);
    neo_usb_host_status_t usb = {0};
    usb_host_neo_get_host_status(&usb);
    bool wifi_up = status.wifi_state == DEVICE_WIFI_CONNECTED;
    printf("{\"wifi\":%s,\"usb_connected\":%s,\"usb_mode\":\"%s\",\"usb_ready\":%s,\"auth\":%s}\n",
           wifi_up ? "true" : "false", usb_host_neo_is_connected() ? "true" : "false", usb_host_neo_get_mode(),
           usb.neo_ready ? "true" : "false", uart_session_valid() ? "true" : "false");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "reboot")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    uart_touch_session();
    printf("Rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_neo_info(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo info")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    cJSON *info = cJSON_CreateObject();
    if (!info) {
        printf("neo info: out of memory\n");
        return 1;
    }
    if (usb_host_neo_get_system_info(info) != ESP_OK) {
        cJSON_Delete(info);
        printf("neo info: query failed\n");
        return 1;
    }
    if (!uart_want_json()) {
        const cJSON *ver = cJSON_GetObjectItemCaseSensitive(info, "version");
        const cJSON *mode = cJSON_GetObjectItemCaseSensitive(info, "mode");
        const cJSON *rom = cJSON_GetObjectItemCaseSensitive(info, "free_rom");
        const cJSON *ram = cJSON_GetObjectItemCaseSensitive(info, "free_ram");
        printf("Neo %s · %s · free ROM %lu · free RAM %lu\n",
               cJSON_IsString(ver) ? ver->valuestring : "?",
               cJSON_IsString(mode) ? mode->valuestring : "?",
               (unsigned long)(cJSON_IsNumber(rom) ? rom->valuedouble : 0),
               (unsigned long)(cJSON_IsNumber(ram) ? ram->valuedouble : 0));
        cJSON_Delete(info);
        return 0;
    }
    return uart_print_cjson(info, "neo info");
}

static int cmd_neo_mode(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo mode")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    const char *mode = usb_host_neo_get_mode();
    bool connected = usb_host_neo_is_connected();
    if (uart_want_json()) {
        printf("{\"mode\":\"%s\",\"connected\":%s}\n", mode, connected ? "true" : "false");
    } else {
        printf("Mode: %s (%s)\n", mode, connected ? "connected" : "not connected");
    }
    return 0;
}

static int cmd_neo_version(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo version")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    char buf[64];
    if (usb_host_neo_get_version(buf, sizeof(buf)) != ESP_OK) {
        printf("neo version: query failed\n");
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"version\":\"%s\"}\n", buf);
    } else {
        printf("Version: %s\n", buf);
    }
    return 0;
}

static int cmd_neo_rescan(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo rescan")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    neo_usb_scan_result_t scan = {0};
    esp_err_t err = usb_host_neo_rescan(&scan);
    if (err != ESP_OK) {
        printf("neo rescan: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"devices_found\":%d,\"neo_ready\":%s,\"flipping\":%s}\n", scan.devices_found,
               scan.neo_ready ? "true" : "false", scan.flipping ? "true" : "false");
    } else {
        printf("Rescan: %d device(s)%s%s\n", scan.devices_found, scan.neo_ready ? ", Neo ready" : "",
               scan.flipping ? ", flipping" : "");
    }
    return 0;
}

static int cmd_neo_restart(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo restart")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    esp_err_t err = usb_host_neo_restart();
    if (err != ESP_OK) {
        printf("neo restart: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Neo restarted (keyboard mode)\n");
    return 0;
}

static int uart_print_alloc_json(esp_err_t (*fill)(char *buf, size_t cap, void *ctx), void *ctx, size_t cap,
                                 const char *fail_msg)
{
    cap = uart_clamp_json_alloc(cap);
    char *buf = malloc(cap);
    if (!buf) {
        printf("%s: out of memory\n", fail_msg);
        return 1;
    }
    esp_err_t err = fill(buf, cap, ctx);
    if (err != ESP_OK) {
        free(buf);
        printf("%s: failed\n", fail_msg);
        return 1;
    }
    printf("%s\n", buf);
    free(buf);
    return 0;
}

static esp_err_t uart_fill_neo_list_applets(char *buf, size_t cap, void *ctx)
{
    (void)ctx;
    return usb_host_neo_list_applets(buf, cap);
}

typedef struct {
    int limit;
} uart_debug_json_ctx_t;

static esp_err_t uart_fill_neo_debug(char *buf, size_t cap, void *ctx)
{
    const uart_debug_json_ctx_t *dbg = (const uart_debug_json_ctx_t *)ctx;
    return neo_debug_get_json(buf, cap, dbg->limit);
}

typedef struct {
    int limit;
} uart_logs_json_ctx_t;

static esp_err_t uart_fill_device_logs(char *buf, size_t cap, void *ctx)
{
    const uart_logs_json_ctx_t *logs = (const uart_logs_json_ctx_t *)ctx;
    return log_buffer_get_recent_json(buf, cap, logs->limit, LOG_LEVEL_INFO);
}

static int cmd_neo_list_applets(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo list-applets")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    return uart_print_alloc_json(uart_fill_neo_list_applets, NULL, UART_JSON_ALLOC_DEFAULT, "neo list-applets");
}

static int cmd_neo_fetch_applet(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo fetch-applet")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 1 || argv[0] == NULL) {
        uart_print_usage("neo fetch-applet");
        return 1;
    }
    uint16_t applet_id = 0;
    if (!parse_u16(argv[0], &applet_id)) {
        printf("neo fetch-applet: invalid applet id\n");
        return 1;
    }

    const size_t cap = 1024 * 1024;
    uint8_t *buf = malloc(cap);
    if (!buf) {
        printf("neo fetch-applet: out of memory\n");
        return 1;
    }
    size_t out_len = 0;
    esp_err_t err = usb_host_neo_fetch_applet(applet_id, buf, cap, &out_len);
    if (err != ESP_OK) {
        free(buf);
        printf("neo fetch-applet: %s\n", esp_err_to_name(err));
        return 1;
    }

    char name[FILE_MANAGER_NAME_MAX];
    name[0] = '\0';
    if (argc >= 2 && argv[1] && argv[1][0]) {
        snprintf(name, sizeof(name), "%s", argv[1]);
    } else if (applet_id == 0) {
        snprintf(name, sizeof(name), "SystemRom.os3kos");
    } else {
        neo_applet_info_t info;
        if (neo_applet_inspect(buf, out_len, &info) == ESP_OK && info.name[0]) {
            size_t o = 0;
            for (size_t i = 0; info.name[i] && o + 12 < sizeof(name); i++) {
                unsigned char c = (unsigned char)info.name[i];
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                    c == '-' || c == '_') {
                    name[o++] = (char)c;
                } else if (c == ' ' || c == '.') {
                    name[o++] = '_';
                }
            }
            name[o] = '\0';
            if (o + 9 < sizeof(name)) {
                memcpy(name + o, ".os3kapp", 9);
            }
        }
        if (name[0] == '\0') {
            snprintf(name, sizeof(name), "applet-%u.os3kapp", (unsigned)applet_id);
        }
    }

    char path[320];
    if (file_manager_ensure_dir() != ESP_OK) {
        free(buf);
        printf("neo fetch-applet: storage not ready\n");
        return 1;
    }
    if (file_manager_resolve_path(name, path, sizeof(path)) != ESP_OK) {
        free(buf);
        printf("neo fetch-applet: invalid filename\n");
        return 1;
    }
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        free(buf);
        printf("neo fetch-applet: open failed %s\n", path);
        return 1;
    }
    size_t wrote = fwrite(buf, 1, out_len, fp);
    if (fflush(fp) != 0) {
        wrote = 0;
    }
    fclose(fp);
    free(buf);
    if (wrote != out_len) {
        unlink(path);
        printf("neo fetch-applet: write failed\n");
        return 1;
    }

    /* Match NeoTools: leave Neo usable as a keyboard after fetch. */
    (void)usb_host_neo_restart();
    if (uart_want_json()) {
        printf("{\"applet_id\":%u,\"bytes\":%u,\"path\":", (unsigned)applet_id, (unsigned)out_len);
        uart_print_json_escaped(path);
        printf("}\n");
    } else {
        printf("Fetched applet 0x%04x (%u bytes) → %s\n", applet_id, (unsigned)out_len, path);
    }
    return 0;
}

static int cmd_neo_list_files(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo list-files")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    uint16_t applet_id = NEO_APPLET_ID_ALPHAWORD;
    if (argc >= 1 && !parse_u16(argv[0], &applet_id)) {
        printf("neo list-files: invalid applet id\n");
        return 1;
    }
    cJSON *files = cJSON_CreateArray();
    if (!files) {
        printf("neo list-files: out of memory\n");
        return 1;
    }
    esp_err_t err = neo_file_list_applet(applet_id, files);
    if (err != ESP_OK) {
        cJSON_Delete(files);
        printf("neo list-files: %s\n", esp_err_to_name(err));
        return 1;
    }
    return uart_print_cjson(files, "neo list-files");
}

static int cmd_neo_file_attrs(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo file-attrs")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 2) {
        uart_print_usage("neo file-attrs");
        return 1;
    }
    uint16_t applet_id = 0;
    uint8_t file_index = 0;
    if (!parse_u16(argv[0], &applet_id) || !parse_u8(argv[1], &file_index)) {
        printf("neo file-attrs: invalid arguments\n");
        return 1;
    }
    neo_file_attr_t attrs;
    esp_err_t err = neo_get_file_attributes(applet_id, file_index, &attrs);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("neo file-attrs: not found\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("neo file-attrs: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("{\"file_index\":%u,\"name\":\"%s\",\"space\":%d,\"alloc_size\":%lu}\n", attrs.file_index, attrs.name,
           attrs.space_number, (unsigned long)attrs.alloc_size);
    return 0;
}

static void uart_print_json_escaped(const char *s)
{
    putchar('"');
    if (s) {
        for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
            if (*p == '"' || *p == '\\') {
                putchar('\\');
                putchar((char)*p);
            } else if (*p < 0x20) {
                printf("\\u%04x", (unsigned)*p);
            } else {
                putchar((char)*p);
            }
        }
    }
    putchar('"');
}

static int cmd_neo_read_file(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo read-file")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 1) {
        uart_print_usage("neo read-file");
        return 1;
    }

    /* Default applet is AlphaWord (0xA000) — the 8 text files live there.
     * Forms:
     *   neo read-file FILE_INDEX [MAX_CHARS]
     *   neo read-file APPLET_ID FILE_INDEX [MAX_CHARS]  (APPLET_ID > 0xFF)
     * MAX_CHARS omitted => return the whole converted story.
     */
    uint16_t applet_id = NEO_APPLET_ID_ALPHAWORD;
    uint8_t file_index = 0;
    size_t max_out = 0; /* 0 = full text */
    int argi = 0;

    uint16_t first = 0;
    if (!parse_u16(argv[0], &first)) {
        printf("neo read-file: invalid arguments\n");
        return 1;
    }
    if (argc >= 2 && first > 0xFF) {
        applet_id = first;
        argi = 1;
    }

    if (!parse_u8(argv[argi], &file_index) || file_index == 0) {
        printf("neo read-file: need FILE_INDEX (1-8 for AlphaWord)\n");
        return 1;
    }
    if (argc > argi + 1) {
        max_out = (size_t)strtoul(argv[argi + 1], NULL, 0);
    }

    neo_file_attr_t attrs;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = usb_host_neo_read_file_alloc(applet_id, file_index, &attrs, &raw, &raw_len, 256 * 1024);
    if (err != ESP_OK) {
        printf("neo read-file: %s\n", err == ESP_ERR_NOT_FOUND ? "not found" : esp_err_to_name(err));
        return 1;
    }

    char *text = NULL;
    size_t text_len = 0;
    char *hex = NULL;

    if (applet_id == NEO_APPLET_ID_ALPHAWORD && raw_len > 0) {
        size_t text_cap = neo_conv_export_buf_size(raw_len);
        text = malloc(text_cap);
        if (!text) {
            free(raw);
            printf("neo read-file: out of memory (need %u bytes for text)\n", (unsigned)text_cap);
            return 1;
        }
        text_len = neo_conv_export_text_from_neo(raw, raw_len, text, text_cap, NEO_CHARMAP_EN_US);
        free(raw);
        raw = NULL;

        if (max_out > 0 && text_len > max_out) {
            text[max_out] = '\0';
            text_len = max_out;
        }
        /* Shrink to exact size so large padded Neo pages don't keep a 2× buffer. */
        char *shrunk = realloc(text, text_len + 1);
        if (shrunk) {
            text = shrunk;
        }
    } else if (raw_len > 0) {
        size_t show = raw_len < 64 ? raw_len : 64;
        hex = malloc(show * 2 + 1);
        if (hex) {
            for (size_t i = 0; i < show; ++i) {
                sprintf(&hex[i * 2], "%02x", raw[i]);
            }
            hex[show * 2] = '\0';
        }
        free(raw);
        raw = NULL;
    } else {
        free(raw);
        raw = NULL;
    }

    /* Stream JSON — do not build a second full copy via cJSON_Print. */
    printf("{\"name\":");
    uart_print_json_escaped(attrs.name);
    printf(",\"file_index\":%u,\"applet_id\":%u,\"bytes\":%u", (unsigned)file_index, (unsigned)applet_id,
           (unsigned)raw_len);
    if (text) {
        printf(",\"text\":");
        uart_print_json_escaped(text);
        printf(",\"text_len\":%u", (unsigned)text_len);
        free(text);
    }
    if (hex) {
        printf(",\"preview_hex\":\"%s\"", hex);
        free(hex);
    }
    printf("}\n");
    return 0;
}

static int uart_neo_backup_all(uint16_t applet_id, const char *cmd_name)
{
    cJSON *saved = cJSON_CreateArray();
    if (!saved) {
        printf("%s: out of memory\n", cmd_name);
        return 1;
    }
    esp_err_t err = usb_host_neo_backup_all_files(applet_id, NEO_CHARMAP_EN_US, saved);
    if (err != ESP_OK) {
        cJSON_Delete(saved);
        printf("%s: %s\n", cmd_name, esp_err_to_name(err));
        return 1;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(saved);
        printf("%s: out of memory\n", cmd_name);
        return 1;
    }
    cJSON_AddItemToObject(root, "saved", saved);
    cJSON_AddNumberToObject(root, "count", cJSON_GetArraySize(saved));
    if (!uart_want_json()) {
        int count = cJSON_GetArraySize(saved);
        printf("Backed up %d file%s\n", count, count == 1 ? "" : "s");
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, saved) {
            const cJSON *path = cJSON_GetObjectItemCaseSensitive(item, "path");
            const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
            if (cJSON_IsString(path)) {
                printf("  %s\n", path->valuestring);
            } else if (cJSON_IsString(name)) {
                printf("  %s\n", name->valuestring);
            }
        }
        cJSON_Delete(root);
        return 0;
    }
    return uart_print_cjson(root, cmd_name);
}

static int uart_neo_backup_one(uint16_t applet_id, uint8_t file_index, const char *cmd_name)
{
    neo_file_attr_t attrs;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = usb_host_neo_read_file_alloc(applet_id, file_index, &attrs, &raw, &raw_len, 256 * 1024);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("%s: file %u not found\n", cmd_name, (unsigned)file_index);
        return 1;
    }
    if (err != ESP_OK) {
        printf("%s: %s\n", cmd_name, esp_err_to_name(err));
        return 1;
    }
    if (raw_len == 0) {
        free(raw);
        printf("%s: file %u is empty\n", cmd_name, (unsigned)file_index);
        return 1;
    }

    char path[256];
    if (applet_id == NEO_APPLET_ID_ALPHAWORD) {
        size_t text_cap = neo_conv_export_buf_size(raw_len);
        char *text = malloc(text_cap);
        if (!text) {
            free(raw);
            printf("%s: out of memory\n", cmd_name);
            return 1;
        }
        size_t text_len = neo_conv_export_text_from_neo(raw, raw_len, text, text_cap, NEO_CHARMAP_EN_US);
        free(raw);
        if (text_len == 0) {
            free(text);
            printf("%s: nothing to save (empty after convert)\n", cmd_name);
            return 1;
        }
        neo_document_t doc = {
            .file_index = file_index,
            .file_name = attrs.name,
            .utf8_text = text,
            .utf8_text_length = text_len,
        };
        err = neo_import_save_document(&doc, path, sizeof(path));
        size_t bytes = text_len;
        free(text);
        if (err == ESP_ERR_NOT_FOUND) {
            printf("%s: skipped blank document\n", cmd_name);
            return 0;
        }
        if (err == ESP_ERR_INVALID_STATE) {
            printf("%s: skipped (identical backup already stored)\n", cmd_name);
            return 0;
        }
        if (err != ESP_OK) {
            printf("%s: save failed: %s\n", cmd_name, esp_err_to_name(err));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"name\":");
            uart_print_json_escaped(attrs.name);
            printf(",\"file_index\":%u,\"path\":", (unsigned)file_index);
            uart_print_json_escaped(path);
            printf(",\"bytes\":%u}\n", (unsigned)bytes);
        } else {
            printf("Saved %s (%u bytes) → %s\n", attrs.name, (unsigned)bytes, path);
        }
        return 0;
    }

    err = neo_import_save_raw_document(raw, raw_len, attrs.name, file_index, path, sizeof(path));
    size_t bytes = raw_len;
    free(raw);
    if (err != ESP_OK) {
        printf("%s: save failed: %s\n", cmd_name, esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"name\":");
        uart_print_json_escaped(attrs.name);
        printf(",\"file_index\":%u,\"path\":", (unsigned)file_index);
        uart_print_json_escaped(path);
        printf(",\"bytes\":%u}\n", (unsigned)bytes);
    } else {
        printf("Saved %s (%u bytes) → %s\n", attrs.name, (unsigned)bytes, path);
    }
    return 0;
}

static int cmd_neo_read_all(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo read-all")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    uint16_t applet_id = NEO_APPLET_ID_ALPHAWORD;
    if (argc >= 1 && !parse_u16(argv[0], &applet_id)) {
        printf("neo read-all: invalid applet id\n");
        return 1;
    }
    return uart_neo_backup_all(applet_id, "neo read-all");
}

static int cmd_neo_backup(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo backup")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }

    uint16_t applet_id = NEO_APPLET_ID_ALPHAWORD;
    uint8_t file_index = 0;
    bool one_file = false;

    if (argc == 0) {
        return uart_neo_backup_all(applet_id, "neo backup");
    }

    uint16_t first = 0;
    if (!parse_u16(argv[0], &first)) {
        printf("neo backup: invalid arguments\n");
        return 1;
    }

    if (first > 0xFF) {
        applet_id = first;
        if (argc >= 2) {
            if (!parse_u8(argv[1], &file_index) || file_index == 0) {
                printf("neo backup: need FILE_INDEX (1-8)\n");
                return 1;
            }
            one_file = true;
        }
    } else {
        if (!parse_u8(argv[0], &file_index) || file_index == 0) {
            printf("neo backup: need FILE_INDEX (1-8) or APPLET_ID\n");
            return 1;
        }
        one_file = true;
    }

    if (one_file) {
        return uart_neo_backup_one(applet_id, file_index, "neo backup");
    }
    return uart_neo_backup_all(applet_id, "neo backup");
}

static int cmd_neo_autobackup(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo autobackup")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    const char *action = (argc >= 1 && argv[0] != NULL) ? argv[0] : "status";
    device_settings_t cfg;
    if (settings_load(&cfg) != ESP_OK) {
        printf("neo autobackup: settings load failed\n");
        return 1;
    }

    if (strcasecmp(action, "status") == 0 || strcasecmp(action, "show") == 0) {
        if (uart_want_json()) {
            printf("{\"auto_backup_on_connect\":%s,\"busy\":%s}\n",
                   cfg.auto_backup_on_connect ? "true" : "false",
                   neo_autobackup_is_busy() ? "true" : "false");
        } else {
            printf("Auto-backup on connect: %s%s\n", cfg.auto_backup_on_connect ? "on" : "off",
                   neo_autobackup_is_busy() ? " (running)" : "");
        }
        return 0;
    }
    if (strcasecmp(action, "on") == 0 || strcasecmp(action, "enable") == 0) {
        cfg.auto_backup_on_connect = true;
        if (settings_save(&cfg) != ESP_OK) {
            printf("neo autobackup: save failed\n");
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"auto_backup_on_connect\":true}\n");
        } else {
            printf("Auto-backup on connect enabled\n");
        }
        return 0;
    }
    if (strcasecmp(action, "off") == 0 || strcasecmp(action, "disable") == 0) {
        cfg.auto_backup_on_connect = false;
        if (settings_save(&cfg) != ESP_OK) {
            printf("neo autobackup: save failed\n");
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"auto_backup_on_connect\":false}\n");
        } else {
            printf("Auto-backup on connect disabled\n");
        }
        return 0;
    }
    if (strcasecmp(action, "now") == 0 || strcasecmp(action, "run") == 0) {
        if (neo_autobackup_is_busy()) {
            printf("neo autobackup: already running\n");
            return 1;
        }
        if (!uart_want_json()) {
            printf("Running backup (changed files), then keyboard mode…\n");
        }
        esp_err_t err = neo_autobackup_run_now(true);
        if (err != ESP_OK) {
            printf("neo autobackup: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"ok\":true,\"returned_to_keyboard\":true}\n");
        } else {
            neo_autobackup_progress_t p;
            neo_autobackup_get_progress(&p);
            printf("Done — saved %u, skipped %u; Neo returned to keyboard\n", (unsigned)p.saved,
                   (unsigned)p.skipped);
        }
        return 0;
    }

    printf("neo autobackup: use on|off|now|status\n");
    return 1;
}

static int cmd_neo_clear_file(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo clear-file")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 2) {
        uart_print_usage("neo clear-file");
        return 1;
    }
    uint16_t applet_id = 0;
    if (!parse_u16(argv[0], &applet_id)) {
        printf("neo clear-file: invalid applet id\n");
        return 1;
    }
    esp_err_t err = usb_host_neo_clear_file_by_name(applet_id, argv[1]);
    if (err == ESP_ERR_NOT_FOUND) {
        printf("neo clear-file: not found\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("neo clear-file: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("File cleared\n");
    return 0;
}

static int cmd_neo_write_file(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo write-file")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 3) {
        uart_print_usage("neo write-file");
        return 1;
    }
    uint16_t applet_id = 0;
    uint8_t file_index = 0;
    if (!parse_u16(argv[0], &applet_id) || !parse_u8(argv[1], &file_index) || file_index == 0) {
        printf("neo write-file: invalid applet id or file index (1..255)\n");
        return 1;
    }
    char text[512];
    uart_join_argv(argc, argv, 2, text, sizeof(text));
    if (text[0] == '\0') {
        printf("neo write-file: empty text\n");
        return 1;
    }
    size_t neo_cap = strlen(text) * 2 + 16;
    uint8_t *neo_buf = malloc(neo_cap);
    if (!neo_buf) {
        printf("neo write-file: out of memory\n");
        return 1;
    }
    size_t neo_len = 0;
    esp_err_t err = neo_conv_import_text_to_neo(text, NEO_CHARMAP_EN_US, neo_buf, neo_cap, &neo_len);
    if (err != ESP_OK) {
        free(neo_buf);
        printf("neo write-file: convert failed (%s)\n", esp_err_to_name(err));
        return 1;
    }
    err = usb_host_neo_write_file_raw(applet_id, file_index, neo_buf, neo_len);
    free(neo_buf);
    if (err != ESP_OK) {
        printf("neo write-file: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"ok\":true,\"bytes\":%u}\n", (unsigned)neo_len);
    } else {
        printf("Wrote %u Neo bytes to applet 0x%04X file %u\n", (unsigned)neo_len, applet_id,
               (unsigned)file_index);
    }
    return 0;
}

static int cmd_neo_space(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo space")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    neo_avail_space_t space = {0};
    if (neo_space_get_available(&space) != ESP_OK) {
        printf("neo space: query failed\n");
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"free_rom\":%lu,\"free_ram\":%lu}\n", (unsigned long)space.free_rom,
               (unsigned long)space.free_ram);
    } else {
        printf("Free ROM %lu · free RAM %lu\n", (unsigned long)space.free_rom,
               (unsigned long)space.free_ram);
    }
    return 0;
}

static int cmd_neo_space_used(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo space-used")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 1) {
        uart_print_usage("neo space-used");
        return 1;
    }
    uint16_t applet_id = 0;
    if (!parse_u16(argv[0], &applet_id)) {
        printf("neo space-used: invalid applet id\n");
        return 1;
    }
    neo_used_space_t used = {0};
    if (neo_space_get_used(applet_id, &used) != ESP_OK) {
        printf("neo space-used: query failed\n");
        return 1;
    }
    printf("{\"ram_used\":%lu,\"file_count\":%u}\n", (unsigned long)used.ram_used, (unsigned)used.file_count);
    return 0;
}

static int cmd_neo_debug(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo debug")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc >= 1 && argv[0] != NULL) {
        if (strcasecmp(argv[0], "on") == 0 || strcasecmp(argv[0], "enable") == 0) {
            neo_debug_set_verbose(true);
            printf("{\"verbose\":true}\n");
            return 0;
        }
        if (strcasecmp(argv[0], "off") == 0 || strcasecmp(argv[0], "disable") == 0) {
            neo_debug_set_verbose(false);
            printf("{\"verbose\":false}\n");
            return 0;
        }
        if (strcasecmp(argv[0], "status") == 0) {
            printf("{\"verbose\":%s}\n", neo_debug_is_verbose() ? "true" : "false");
            return 0;
        }
    }
    int limit = 20;
    if (argc >= 1) {
        limit = atoi(argv[0]);
        if (limit <= 0 || limit > 100) {
            limit = 20;
        }
    }
    uart_debug_json_ctx_t ctx = {.limit = limit};
    return uart_print_alloc_json(uart_fill_neo_debug, &ctx, UART_JSON_ALLOC_DEFAULT, "neo debug");
}

static int cmd_neo_remove_applet(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo remove-applet")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    if (argc < 1) {
        uart_print_usage("neo remove-applet");
        return 1;
    }
    uint16_t applet_id = 0;
    if (!parse_u16(argv[0], &applet_id)) {
        printf("neo remove-applet: invalid applet id\n");
        return 1;
    }
    esp_err_t err = usb_host_neo_remove_applet(applet_id);
    if (err != ESP_OK) {
        printf("neo remove-applet: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Applet removed\n");
    return 0;
}

static int cmd_neo_remove_all(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "neo remove-all")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (uart_neo_require() != ESP_OK) {
        return 1;
    }
    esp_err_t err = usb_host_neo_remove_all_applets();
    if (err != ESP_OK) {
        printf("neo remove-all: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("All applets removed\n");
    return 0;
}

static int cmd_device_info(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device info")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    uint64_t uptime_sec = esp_timer_get_time() / 1000000ULL;
    printf("{\"chip\":\"%s\",\"cores\":%u,\"revision\":%u,\"idf\":\"%s\",\"flash_bytes\":%lu,"
           "\"reset\":\"%s\",\"uptime_sec\":%llu,\"free_heap\":%lu,\"min_free_heap\":%lu}\n",
           device_chip_model_str(), chip.cores, chip.revision, esp_get_idf_version(), (unsigned long)flash_size,
           device_reset_reason_str(esp_reset_reason()), (unsigned long long)uptime_sec,
           (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size());
    return 0;
}

static int cmd_device_wifi(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device wifi")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    const char *action = (argc >= 1 && argv[0] != NULL) ? argv[0] : "status";

    if (strcasecmp(action, "status") == 0 || strcasecmp(action, "show") == 0) {
        device_status_t st = {0};
        device_status_get(&st);
        device_settings_t cfg;
        settings_load(&cfg);
        const char *mode = cfg.network_mode == SETTINGS_NETWORK_HOME ? "home" : "direct";
        printf("{\"network_mode\":\"%s\",\"wifi_state\":\"%s\",\"connected\":%s,\"recovery\":%s,"
               "\"ssid\":\"%s\",\"ip\":\"%s\",\"dhcp\":%s}\n",
               mode, device_wifi_state_str(st.wifi_state), wifi_manager_is_connected() ? "true" : "false",
               wifi_manager_is_recovery_mode() ? "true" : "false", st.wifi_ssid, st.ip_address,
               cfg.wifi_dhcp ? "true" : "false");
        return 0;
    }

    if (strcasecmp(action, "scan") == 0) {
#if HAVE_WIFI_WEB
        esp_err_t err = esp_wifi_scan_start(NULL, true);
        if (err != ESP_OK) {
            printf("device wifi scan: %s\n", esp_err_to_name(err));
            return 1;
        }
        uint16_t ap_count = 32;
        wifi_ap_record_t ap_records[32];
        if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) != ESP_OK) {
            printf("device wifi scan: read failed\n");
            return 1;
        }
        cJSON *arr = cJSON_CreateArray();
        if (!arr) {
            printf("device wifi scan: out of memory\n");
            return 1;
        }
        for (uint16_t i = 0; i < ap_count; ++i) {
            if (ap_records[i].ssid[0] == '\0') {
                continue;
            }
            cJSON *it = cJSON_CreateObject();
            if (!it) {
                continue;
            }
            cJSON_AddStringToObject(it, "ssid", (const char *)ap_records[i].ssid);
            cJSON_AddNumberToObject(it, "rssi", ap_records[i].rssi);
            cJSON_AddNumberToObject(it, "channel", ap_records[i].primary);
            cJSON_AddItemToArray(arr, it);
        }
        return uart_print_cjson(arr, "device wifi scan");
#else
        printf("device wifi scan: Wi-Fi/web disabled in this build\n");
        return 1;
#endif
    }

    if (strcasecmp(action, "connect") == 0) {
        char ssid[33];
        char password[65];
        if (!uart_parse_wifi_credentials(argc, argv, 1, ssid, sizeof(ssid), password, sizeof(password))) {
            printf("device wifi connect: missing SSID\n");
            printf("Use: device wifi connect \"My SSID\" \"password\"\n");
            printf("  Open network: device wifi connect \"My SSID\" \"\"\n");
            printf("  (single quotes also work after this firmware build)\n");
            return 1;
        }
        device_settings_t cfg;
        if (settings_load(&cfg) != ESP_OK) {
            printf("device wifi connect: settings load failed\n");
            return 1;
        }
        strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
        strlcpy(cfg.wifi_password, password, sizeof(cfg.wifi_password));
        (void)settings_wifi_upsert(&cfg, ssid, password);
        cfg.network_mode = SETTINGS_NETWORK_HOME;
        if (settings_save(&cfg) != ESP_OK) {
            printf("device wifi connect: save failed\n");
            return 1;
        }
        esp_err_t err = wifi_manager_connect(cfg.wifi_ssid, cfg.wifi_password);
        if (err != ESP_OK) {
            printf("device wifi connect: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"status\":\"connecting\",\"ssid\":\"%s\"}\n", cfg.wifi_ssid);
        } else {
            printf("Connecting to \"%s\"…\n", cfg.wifi_ssid);
        }
        return 0;
    }

    if (strcasecmp(action, "mode") == 0) {
        if (argc < 2 || argv[1] == NULL) {
            printf("device wifi mode: use home|direct\n");
            return 1;
        }
        device_settings_t cfg;
        if (settings_load(&cfg) != ESP_OK) {
            printf("device wifi mode: settings load failed\n");
            return 1;
        }
        if (strcasecmp(argv[1], "home") == 0 || strcasecmp(argv[1], "sta") == 0) {
            cfg.network_mode = SETTINGS_NETWORK_HOME;
            if (settings_save(&cfg) != ESP_OK) {
                printf("device wifi mode: save failed\n");
                return 1;
            }
            wifi_manager_connect(cfg.wifi_ssid, cfg.wifi_password);
            printf("Network mode: home Wi-Fi\n");
            return 0;
        }
        if (strcasecmp(argv[1], "direct") == 0 || strcasecmp(argv[1], "ap") == 0) {
            cfg.network_mode = SETTINGS_NETWORK_DIRECT;
            settings_apply_hotspot_defaults(&cfg);
            if (settings_save(&cfg) != ESP_OK) {
                printf("device wifi mode: save failed\n");
                return 1;
            }
            wifi_manager_start_ap();
            printf("Network mode: direct hotspot (%s)\n", cfg.hotspot_ssid);
            return 0;
        }
        printf("device wifi mode: use home|direct\n");
        return 1;
    }

    if (strcasecmp(action, "ap") == 0) {
        device_settings_t cfg;
        if (settings_load(&cfg) != ESP_OK) {
            printf("device wifi ap: settings load failed\n");
            return 1;
        }
        cfg.network_mode = SETTINGS_NETWORK_DIRECT;
        settings_apply_hotspot_defaults(&cfg);
        settings_save(&cfg);
        esp_err_t err = wifi_manager_start_ap();
        if (err != ESP_OK) {
            printf("device wifi ap: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("Hotspot started (%s)\n", cfg.hotspot_ssid);
        return 0;
    }

    printf("device wifi: use status|scan|connect SSID [PASSWORD]|mode home|direct|ap\n");
    return 1;
}

static int cmd_device_wifi_recovery(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device wifi-recovery")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    esp_err_t err = wifi_manager_force_recovery_ap();
    if (err != ESP_OK) {
        printf("device wifi-recovery: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Recovery hotspot started\n");
    return 0;
}

static int cmd_device_storage(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device storage")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    size_t spiffs_total = 0;
    size_t spiffs_used = 0;
    esp_spiffs_info(NULL, &spiffs_total, &spiffs_used);
    device_status_t st = {0};
    device_status_get(&st);
    printf("{\"spiffs_total\":%lu,\"spiffs_used\":%lu,\"sd_mounted\":%s,\"sd_total\":%lu,\"sd_used\":%lu}\n",
           (unsigned long)spiffs_total, (unsigned long)spiffs_used, st.sd_card_mounted ? "true" : "false",
           (unsigned long)st.sd_card_total_bytes, (unsigned long)st.sd_card_used_bytes);
    return 0;
}

static int cmd_device_sd_remount(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device sd-remount")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
#if HAVE_SDCARD
    esp_err_t err = sd_card_mount_if_present();
    if (err != ESP_OK) {
        printf("device sd-remount: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("SD card mounted at %s\n", SD_CARD_MOUNT_PATH);
    return 0;
#else
    printf("device sd-remount: SD card support not compiled in\n");
    return 1;
#endif
}

static int cmd_device_sd_format(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device sd-format")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
#if HAVE_SDCARD
    esp_err_t err = sd_format_start();
    if (err == ESP_ERR_INVALID_STATE) {
        printf("device sd-format: already formatting\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("device sd-format: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"started\":true}\n");
    } else {
        printf("SD format started (destructive)\n");
    }
    return 0;
#else
    (void)argc;
    (void)argv;
    printf("device sd-format: SD card support not compiled in\n");
    return 1;
#endif
}

static int cmd_device_logs(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device logs")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    int limit = 30;
    if (argc >= 1) {
        limit = atoi(argv[0]);
        if (limit <= 0 || limit > 200) {
            limit = 30;
        }
    }
    uart_logs_json_ctx_t ctx = {.limit = limit};
    return uart_print_alloc_json(uart_fill_device_logs, &ctx, UART_JSON_ALLOC_MAX, "device logs");
}

static int uart_settings_print(const device_settings_t *cfg)
{
    const char *mode = cfg->network_mode == SETTINGS_NETWORK_HOME ? "home" : "direct";
    printf("{\"device_name\":\"%s\",\"network_mode\":\"%s\",\"onboarding_complete\":%s,"
           "\"display_brightness\":%u,\"sleep_timeout_sec\":%u,\"keyboard_layout\":\"%s\","
           "\"require_portal_auth\":%s,\"auto_backup_on_connect\":%s,\"auto_cloud_sync_after_backup\":%s,"
           "\"neo_label\":\"%s\",\"wifi_ssid\":\"%s\",\"wifi_dhcp\":%s,\"wifi_ip\":\"%s\","
           "\"wifi_netmask\":\"%s\",\"wifi_gateway\":\"%s\",\"wifi_dns\":\"%s\","
           "\"hotspot_ssid\":\"%s\",\"wifi_password_set\":%s,\"hotspot_password_set\":%s}\n",
           cfg->device_name, mode, cfg->onboarding_complete ? "true" : "false", cfg->display_brightness,
           cfg->sleep_timeout_seconds, cfg->keyboard_layout, cfg->require_portal_auth ? "true" : "false",
           cfg->auto_backup_on_connect ? "true" : "false",
           cfg->auto_cloud_sync_after_backup ? "true" : "false", cfg->neo_label, cfg->wifi_ssid,
           cfg->wifi_dhcp ? "true" : "false", cfg->wifi_ip, cfg->wifi_netmask, cfg->wifi_gateway,
           cfg->wifi_dns, cfg->hotspot_ssid, cfg->wifi_password[0] ? "true" : "false",
           cfg->hotspot_password[0] ? "true" : "false");
    return 0;
}

static int cmd_device_settings(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device settings")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    device_settings_t cfg;
    if (settings_load(&cfg) != ESP_OK) {
        printf("device settings: load failed\n");
        return 1;
    }
    settings_apply_hotspot_defaults(&cfg);

    if (argc < 1 || argv[0] == NULL || strcasecmp(argv[0], "show") == 0 ||
        strcasecmp(argv[0], "get") == 0) {
        return uart_settings_print(&cfg);
    }

    if (strcasecmp(argv[0], "set") != 0) {
        printf("device settings: use show|set KEY VALUE\n");
        return 1;
    }
    if (argc < 3 || argv[1] == NULL) {
        printf("device settings set: missing KEY VALUE\n");
        return 1;
    }

    const char *key = argv[1];
    char value[192];
    uart_join_argv(argc, argv, 2, value, sizeof(value));
    device_settings_t prev = cfg;
    bool network_changed = false;

    if (strcasecmp(key, "device_name") == 0) {
        if (value[0] == '\0' || strlen(value) >= SETTINGS_DEVICE_NAME_MAX_LENGTH) {
            printf("device settings: invalid device_name length\n");
            return 1;
        }
        strlcpy(cfg.device_name, value, sizeof(cfg.device_name));
    } else if (strcasecmp(key, "keyboard_layout") == 0) {
        strlcpy(cfg.keyboard_layout, value, sizeof(cfg.keyboard_layout));
    } else if (strcasecmp(key, "display_brightness") == 0) {
        unsigned long v = strtoul(value, NULL, 10);
        if (v > 100) {
            printf("device settings: brightness 0..100\n");
            return 1;
        }
        cfg.display_brightness = (uint8_t)v;
    } else if (strcasecmp(key, "sleep_timeout_seconds") == 0 || strcasecmp(key, "sleep_timeout") == 0) {
        cfg.sleep_timeout_seconds = (uint16_t)strtoul(value, NULL, 10);
    } else if (strcasecmp(key, "require_portal_auth") == 0) {
        bool b;
        if (!uart_parse_bool(value, &b)) {
            printf("device settings: require_portal_auth needs on|off\n");
            return 1;
        }
        cfg.require_portal_auth = b;
    } else if (strcasecmp(key, "auto_backup_on_connect") == 0) {
        bool b;
        if (!uart_parse_bool(value, &b)) {
            printf("device settings: auto_backup_on_connect needs on|off\n");
            return 1;
        }
        cfg.auto_backup_on_connect = b;
    } else if (strcasecmp(key, "auto_cloud_sync_after_backup") == 0) {
        bool b;
        if (!uart_parse_bool(value, &b)) {
            printf("device settings: auto_cloud_sync_after_backup needs on|off\n");
            return 1;
        }
        cfg.auto_cloud_sync_after_backup = b;
    } else if (strcasecmp(key, "neo_label") == 0) {
        strlcpy(cfg.neo_label, value, sizeof(cfg.neo_label));
    } else if (strcasecmp(key, "network_mode") == 0) {
        if (strcasecmp(value, "home") == 0) {
            cfg.network_mode = SETTINGS_NETWORK_HOME;
        } else if (strcasecmp(value, "direct") == 0) {
            cfg.network_mode = SETTINGS_NETWORK_DIRECT;
        } else {
            printf("device settings: network_mode home|direct\n");
            return 1;
        }
        network_changed = true;
    } else if (strcasecmp(key, "hotspot_ssid") == 0) {
        strlcpy(cfg.hotspot_ssid, value, sizeof(cfg.hotspot_ssid));
        network_changed = true;
    } else if (strcasecmp(key, "hotspot_password") == 0) {
        if (strlen(value) < 8) {
            printf("device settings: hotspot_password must be at least 8 characters\n");
            return 1;
        }
        strlcpy(cfg.hotspot_password, value, sizeof(cfg.hotspot_password));
        network_changed = true;
    } else if (strcasecmp(key, "wifi_ssid") == 0) {
        strlcpy(cfg.wifi_ssid, value, sizeof(cfg.wifi_ssid));
        network_changed = true;
    } else if (strcasecmp(key, "wifi_password") == 0) {
        strlcpy(cfg.wifi_password, value, sizeof(cfg.wifi_password));
        network_changed = true;
    } else if (strcasecmp(key, "wifi_dhcp") == 0) {
        bool b;
        if (!uart_parse_bool(value, &b)) {
            printf("device settings: wifi_dhcp needs on|off\n");
            return 1;
        }
        cfg.wifi_dhcp = b;
        network_changed = true;
    } else if (strcasecmp(key, "wifi_ip") == 0) {
        strlcpy(cfg.wifi_ip, value, sizeof(cfg.wifi_ip));
        network_changed = true;
    } else if (strcasecmp(key, "wifi_netmask") == 0) {
        strlcpy(cfg.wifi_netmask, value, sizeof(cfg.wifi_netmask));
        network_changed = true;
    } else if (strcasecmp(key, "wifi_gateway") == 0) {
        strlcpy(cfg.wifi_gateway, value, sizeof(cfg.wifi_gateway));
        network_changed = true;
    } else if (strcasecmp(key, "wifi_dns") == 0) {
        strlcpy(cfg.wifi_dns, value, sizeof(cfg.wifi_dns));
        network_changed = true;
    } else {
        printf("device settings: unknown key '%s'\n", key);
        return 1;
    }

    settings_apply_hotspot_defaults(&cfg);
    if (settings_save(&cfg) != ESP_OK) {
        printf("device settings: save failed\n");
        return 1;
    }
#if HAVE_OLED
    display_set_brightness(cfg.display_brightness);
#else
    (void)0;
#endif

    if (network_changed) {
        if (cfg.network_mode == SETTINGS_NETWORK_HOME) {
            wifi_manager_connect(cfg.wifi_ssid, cfg.wifi_password);
        } else {
            wifi_manager_start_ap();
        }
    }

    (void)prev;
    if (uart_want_json()) {
        printf("{\"ok\":true,\"key\":\"%s\"}\n", key);
    } else {
        printf("Setting %s updated\n", key);
    }
    return 0;
}

static int uart_ble_print_status(void)
{
    device_status_t st = {0};
    device_status_get(&st);
    const char *state = "idle";
    if (st.ble_state == DEVICE_BLE_PAIRING) {
        state = "pairing";
    } else if (st.ble_state == DEVICE_BLE_CONNECTED) {
        state = "connected";
    }

    ble_hid_bond_peer_t peers[BLE_HID_MAX_BONDS];
    int n = ble_hid_list_bonds(peers, BLE_HID_MAX_BONDS);

    if (uart_want_json()) {
        printf("{\"state\":\"%s\",\"connected\":%s,\"advertising\":%s,\"pairing_enabled\":%s,"
               "\"can_send\":%s,\"send_in_progress\":%s,\"bonded\":%d,\"passthrough\":true,"
               "\"bonds\":[",
               state, ble_hid_is_connected() ? "true" : "false",
               ble_hid_is_advertising() ? "true" : "false",
               ble_hid_pairing_enabled() ? "true" : "false",
               ble_hid_can_send() ? "true" : "false",
               ble_hid_send_in_progress() ? "true" : "false", n);
        for (int i = 0; i < n; ++i) {
            const uint8_t *a = peers[i].addr;
            printf("%s{\"addr\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"type\":%u}",
                   i ? "," : "", a[0], a[1], a[2], a[3], a[4], a[5],
                   (unsigned)peers[i].type);
        }
        printf("]}\n");
        return 0;
    }

    printf("BLE state=%s connected=%s advertising=%s pairing=%s bonded=%d\n",
           state,
           ble_hid_is_connected() ? "yes" : "no",
           ble_hid_is_advertising() ? "yes" : "no",
           ble_hid_pairing_enabled() ? "yes" : "no",
           n);
    if (n == 0) {
        printf("  (no bonded hosts)\n");
    } else {
        for (int i = 0; i < n; ++i) {
            const uint8_t *a = peers[i].addr;
            const char *kind = "other";
            if (peers[i].type == 0) {
                kind = "public";
            } else if (peers[i].type == 1) {
                kind = "random";
            }
            printf("  %d. %02x:%02x:%02x:%02x:%02x:%02x (%s)\n", i + 1,
                   a[0], a[1], a[2], a[3], a[4], a[5], kind);
        }
    }
    return 0;
}

static int cmd_device_ble(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device ble")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    if (argc < 1 || argv[0] == NULL || strcasecmp(argv[0], "status") == 0) {
        return uart_ble_print_status();
    }

    const char *action = argv[0];

    if (strcasecmp(action, "pair") == 0) {
        if (argc < 2 || argv[1] == NULL) {
            printf("device ble pair: use on|off\n");
            return 1;
        }
        bool enabled;
        if (strcasecmp(argv[1], "on") == 0 || strcasecmp(argv[1], "enable") == 0) {
            enabled = true;
        } else if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "disable") == 0) {
            enabled = false;
        } else {
            printf("device ble pair: use on|off\n");
            return 1;
        }
        esp_err_t perr = ble_hid_set_pairing_enabled(enabled);
        if (uart_want_json()) {
            if (perr != ESP_OK) {
                size_t largest =
                    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
                printf("{\"pairing\":false,\"error\":\"no_mem\",\"largest_internal\":%u}\n",
                       (unsigned)largest);
            } else {
                printf("{\"pairing\":%s,\"advertising\":%s,\"ready\":%s}\n",
                       enabled ? "true" : "false",
                       ble_hid_is_advertising() ? "true" : "false",
                       ble_hid_is_ready() ? "true" : "false");
            }
        } else if (perr != ESP_OK) {
            printf("BLE pairing failed (not enough free memory)\n");
        } else {
            printf("BLE pairing %s\n", enabled ? "enabled" : "disabled");
        }
        return perr == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(action, "clear") == 0 ||
        strcasecmp(action, "unpair") == 0 ||
        (strcasecmp(action, "bonds") == 0 && argc >= 2 && argv[1] &&
         (strcasecmp(argv[1], "clear") == 0 || strcasecmp(argv[1], "forget") == 0))) {
        int before = ble_hid_bonded_count();
        ble_hid_clear_bonds();
        if (uart_want_json()) {
            printf("{\"cleared\":true,\"bonded_before\":%d,\"bonded\":%d}\n",
                   before, ble_hid_bonded_count());
        } else {
            printf("Cleared %d bonded Bluetooth host%s\n",
                   before, before == 1 ? "" : "s");
        }
        return 0;
    }

    if (strcasecmp(action, "bonds") == 0 || strcasecmp(action, "list") == 0) {
        return uart_ble_print_status();
    }

    if (strcasecmp(action, "preview") == 0) {
        if (argc < 2) {
            printf("device ble preview: missing text\n");
            return 1;
        }
        char text[256];
        text[0] = '\0';
        for (int i = 1; i < argc; ++i) {
            if (argv[i] == NULL) {
                continue;
            }
            if (i > 1) {
                strlcat(text, " ", sizeof(text));
            }
            strlcat(text, argv[i], sizeof(text));
        }
        if (text[0] == '\0') {
            printf("device ble preview: empty text\n");
            return 1;
        }
        char preview[256];
        size_t total = 0;
        esp_err_t err = ble_hid_preview_text(text, preview, sizeof(preview), &total);
        if (err != ESP_OK) {
            if (err == ESP_ERR_INVALID_STATE) {
                printf("device ble preview: send already in progress\n");
            } else if (err == ESP_ERR_INVALID_SIZE) {
                printf("device ble preview: text empty or too long\n");
            } else {
                printf("device ble preview: %s\n", esp_err_to_name(err));
            }
            return 1;
        }
        if (uart_want_json()) {
            cJSON *resp = cJSON_CreateObject();
            if (!resp) {
                printf("device ble preview: out of memory\n");
                return 1;
            }
            cJSON_AddStringToObject(resp, "preview", preview);
            cJSON_AddNumberToObject(resp, "length", (double)total);
            cJSON_AddBoolToObject(resp, "can_send", ble_hid_can_send() ? 1 : 0);
            cJSON_AddBoolToObject(resp, "needs_host", ble_hid_can_send() ? 0 : 1);
            return uart_print_cjson(resp, "device ble preview");
        }
        printf("Preview (%u chars): %s%s\n", (unsigned)total, preview,
               total > strlen(preview) ? "…" : "");
        if (!ble_hid_can_send()) {
            printf("Pair a BLE host before sending.\n");
        }
        return 0;
    }

    if (strcasecmp(action, "send") == 0) {
        esp_err_t err = ble_hid_confirm_send();
        if (err == ESP_ERR_INVALID_STATE) {
            printf("device ble send: not connected or no preview queued\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("device ble send: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"started\":true}\n");
        } else {
            printf("BLE send started\n");
        }
        return 0;
    }

    if (strcasecmp(action, "cancel") == 0) {
        ble_hid_cancel_send();
        if (uart_want_json()) {
            printf("{\"cancelled\":true}\n");
        } else {
            printf("BLE send cancelled\n");
        }
        return 0;
    }

    printf("device ble: use status|bonds|pair on|off|clear|preview TEXT|send|cancel\n");
    return 1;
}

static int cmd_device_battery(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device battery")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
#if HAVE_BATTERY
    device_status_t st = {0};
    device_status_get(&st);
    printf("{\"millivolts\":%u,\"percent\":%u,\"charging\":%s}\n", st.battery_mv, st.battery_percent,
           st.charging ? "true" : "false");
    return 0;
#else
    printf("device battery: not available on this build\n");
    return 1;
#endif
}

static int cmd_device_heap(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device heap")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    printf("{\"free_heap\":%lu,\"min_free_heap\":%lu,\"largest_block\":%lu}\n",
           (unsigned long)esp_get_free_heap_size(), (unsigned long)esp_get_minimum_free_heap_size(),
           (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    return 0;
}

static int cmd_device_name(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device name")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc < 1) {
        printf("{\"device_name\":\"%s\"}\n", settings_get_device_name());
        return 0;
    }
    char new_name[SETTINGS_DEVICE_NAME_MAX_LENGTH + 1];
    new_name[0] = '\0';
    for (int i = 0; i < argc; ++i) {
        if (argv[i] == NULL) {
            continue;
        }
        if (i > 0) {
            strlcat(new_name, " ", sizeof(new_name));
        }
        strlcat(new_name, argv[i], sizeof(new_name));
    }
    size_t len = strlen(new_name);
    if (len == 0 || len >= SETTINGS_DEVICE_NAME_MAX_LENGTH) {
        printf("device name: invalid length (max %d)\n", SETTINGS_DEVICE_NAME_MAX_LENGTH - 1);
        return 1;
    }
    device_settings_t cfg;
    if (settings_load(&cfg) != ESP_OK) {
        printf("device name: load failed\n");
        return 1;
    }
    strlcpy(cfg.device_name, new_name, sizeof(cfg.device_name));
    if (settings_save(&cfg) != ESP_OK) {
        printf("device name: save failed\n");
        return 1;
    }
    printf("Device name set to \"%s\"\n", cfg.device_name);
    return 0;
}

static int cmd_device_reboot(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device reboot")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    uart_touch_session();
    printf("Rebooting...\n");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return 0;
}

static int cmd_device_factory_reset(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device factory-reset")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0') {
        uart_print_usage("device factory-reset");
        return 1;
    }
    char err[96];
    esp_err_t result = factory_reset_perform(argv[1], err, sizeof(err));
    if (result == ESP_ERR_INVALID_STATE) {
        printf("factory-reset: %s\n", err[0] ? err : "too many attempts");
        return 1;
    }
    if (result != ESP_OK) {
        printf("factory-reset: %s\n", err[0] ? err : "invalid password");
        return 1;
    }
    return 0;
}

static int cmd_device_password(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device password")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc < 2 || argv[0] == NULL || argv[1] == NULL) {
        uart_print_usage("device password");
        return 1;
    }
    esp_err_t err = auth_change_password(argv[0], argv[1]);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("device password: current password wrong or new password invalid (8..63 chars)\n");
        return 1;
    }
    if (err != ESP_OK) {
        printf("device password: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"ok\":true}\n");
    } else {
        printf("Password updated\n");
    }
    return 0;
}

static int cmd_device_sync(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "device sync")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    const char *action = (argc >= 1 && argv[0] != NULL) ? argv[0] : "status";

    if (strcasecmp(action, "status") == 0 || strcasecmp(action, "show") == 0 ||
        strcasecmp(action, "config") == 0) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            printf("device sync: out of memory\n");
            return 1;
        }
        if (cloud_sync_json_config(root) != ESP_OK) {
            cJSON_Delete(root);
            printf("device sync: config failed\n");
            return 1;
        }
        return uart_print_cjson(root, "device sync");
    }

    if (strcasecmp(action, "set") == 0) {
        if (argc < 3 || argv[1] == NULL) {
            printf("device sync set: missing KEY VALUE\n");
            return 1;
        }
        const char *key = argv[1];
        char value[256];
        uart_join_argv(argc, argv, 2, value, sizeof(value));
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            printf("device sync set: out of memory\n");
            return 1;
        }
        if (strcasecmp(key, "enabled") == 0) {
            bool b;
            if (!uart_parse_bool(value, &b)) {
                cJSON_Delete(root);
                printf("device sync set: enabled needs on|off\n");
                return 1;
            }
            cJSON_AddBoolToObject(root, "enabled", b ? 1 : 0);
        } else if (strcasecmp(key, "provider") == 0 || strcasecmp(key, "endpoint") == 0 ||
                   strcasecmp(key, "folder") == 0 || strcasecmp(key, "path") == 0 ||
                   strcasecmp(key, "bucket") == 0 || strcasecmp(key, "region") == 0 ||
                   strcasecmp(key, "username") == 0 || strcasecmp(key, "secret") == 0) {
            cJSON_AddStringToObject(root, key, value);
        } else {
            cJSON_Delete(root);
            printf("device sync set: unknown key '%s'\n", key);
            return 1;
        }
        char err[128] = {0};
        esp_err_t code = cloud_sync_apply_config_json(root, err, sizeof(err));
        cJSON_Delete(root);
        if (code != ESP_OK) {
            printf("device sync set: %s\n", err[0] ? err : esp_err_to_name(code));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"ok\":true,\"key\":\"%s\"}\n", key);
        } else {
            printf("Sync %s updated\n", key);
        }
        return 0;
    }

    if (strcasecmp(action, "test") == 0) {
        char message[160];
        esp_err_t err = cloud_sync_test(message, sizeof(message));
        if (uart_want_json()) {
            printf("{\"ok\":%s,\"message\":\"%s\"}\n", err == ESP_OK ? "true" : "false", message);
        } else {
            printf("Sync test %s: %s\n", err == ESP_OK ? "ok" : "failed", message);
        }
        return err == ESP_OK ? 0 : 1;
    }

    if (strcasecmp(action, "run") == 0) {
        esp_err_t err = cloud_sync_start_run();
        if (err == ESP_ERR_INVALID_STATE) {
            printf("device sync run: already busy\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("device sync run: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"started\":true}\n");
        } else {
            printf("Cloud sync started\n");
        }
        return 0;
    }

    printf("device sync: use status|config|set KEY VALUE|test|run\n");
    return 1;
}

#if CONFIG_BUDDY_NEO_LINK
static int cmd_link_llm(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "link llm")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    const char *action = (argc >= 1 && argv[0] != NULL) ? argv[0] : "status";

    if (strcasecmp(action, "status") == 0 || strcasecmp(action, "show") == 0 ||
        strcasecmp(action, "config") == 0) {
        neo_link_llm_config_t cfg;
        neo_link_llm_load(&cfg);
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            printf("link llm: out of memory\n");
            return 1;
        }
        neo_link_llm_status_json(root);
        cJSON_AddStringToObject(root, "system", cfg.system);
        return uart_print_cjson(root, "link llm");
    }

    if (strcasecmp(action, "set") == 0) {
        if (argc < 3 || argv[1] == NULL) {
            printf("link llm set: missing KEY VALUE\n");
            return 1;
        }
        const char *key = argv[1];
        char value[NEO_LINK_LLM_SYSTEM_MAX];
        uart_join_argv(argc, argv, 2, value, sizeof(value));

        neo_link_llm_config_t cfg;
        if (neo_link_llm_load(&cfg) != ESP_OK) {
            neo_link_llm_defaults(&cfg);
        }

        if (strcasecmp(key, "enabled") == 0) {
            bool b;
            if (!uart_parse_bool(value, &b)) {
                printf("link llm set: enabled needs on|off\n");
                return 1;
            }
            cfg.enabled = b;
        } else if (strcasecmp(key, "base_url") == 0) {
            strlcpy(cfg.base_url, value, sizeof(cfg.base_url));
        } else if (strcasecmp(key, "api_key") == 0) {
            strlcpy(cfg.api_key, value, sizeof(cfg.api_key));
        } else if (strcasecmp(key, "model") == 0) {
            strlcpy(cfg.model, value, sizeof(cfg.model));
        } else if (strcasecmp(key, "system") == 0) {
            strlcpy(cfg.system, value, sizeof(cfg.system));
        } else if (strcasecmp(key, "max_tokens") == 0) {
            cfg.max_tokens = (uint16_t)atoi(value);
        } else if (strcasecmp(key, "max_rpm") == 0) {
            cfg.max_rpm = (uint8_t)atoi(value);
        } else if (strcasecmp(key, "context_turns") == 0 || strcasecmp(key, "ctx_turns") == 0) {
            cfg.context_turns = (uint8_t)atoi(value);
        } else {
            printf("link llm set: unknown key '%s'\n", key);
            return 1;
        }

        esp_err_t err = neo_link_llm_save(&cfg);
        if (err != ESP_OK) {
            printf("link llm set: %s\n", esp_err_to_name(err));
            return 1;
        }
        if (uart_want_json()) {
            printf("{\"ok\":true,\"key\":\"%s\"}\n", key);
        } else if (strcasecmp(key, "api_key") == 0) {
            printf("LLM api_key %s\n", cfg.api_key[0] ? "updated" : "cleared");
        } else {
            printf("LLM %s updated\n", key);
        }
        return 0;
    }

    if (strcasecmp(action, "test") == 0) {
        char prompt[256] = "Reply with exactly: Neo Link OK";
        if (argc >= 2) {
            uart_join_argv(argc, argv, 1, prompt, sizeof(prompt));
        }
        char reply[1201];
        char errbuf[96];
        esp_err_t err = neo_link_llm_chat(prompt, reply, sizeof(reply), errbuf, sizeof(errbuf));
        if (uart_want_json()) {
            cJSON *root = cJSON_CreateObject();
            if (!root) {
                printf("{\"ok\":false,\"error\":\"oom\"}\n");
                return 1;
            }
            cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
            if (err == ESP_OK) {
                cJSON_AddStringToObject(root, "reply", reply);
            } else {
                cJSON_AddStringToObject(root, "error", errbuf[0] ? errbuf : esp_err_to_name(err));
            }
            return uart_print_cjson(root, "link llm test");
        }
        if (err != ESP_OK) {
            printf("link llm test failed: %s\n", errbuf[0] ? errbuf : esp_err_to_name(err));
            return 1;
        }
        printf("link llm test ok:\n%s\n", reply);
        return 0;
    }

    if (strcasecmp(action, "clear-context") == 0 || strcasecmp(action, "clear_context") == 0) {
        neo_link_llm_clear_context();
        if (uart_want_json()) {
            printf("{\"ok\":true}\n");
        } else {
            printf("LLM context cleared\n");
        }
        return 0;
    }

    printf("link llm: use status|set KEY VALUE|test [PROMPT]|clear-context\n");
    return 1;
}

static int cmd_link_install(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "link install")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    bool replace = true;
    for (int i = 0; i < argc; ++i) {
        if (argv[i] && (strcasecmp(argv[i], "--no-replace") == 0 || strcasecmp(argv[i], "no-replace") == 0)) {
            replace = false;
        }
    }
    if (!usb_host_neo_is_connected()) {
        printf("link install: Neo not connected\n");
        return 1;
    }
    size_t len = 0;
    (void)neo_link_applet_blob(&len);
    printf("Installing BetaWise HelloWorld (%u bytes)%s…\n", (unsigned)len, replace ? ", replace" : "");
    esp_err_t err = neo_link_applet_ensure_current(replace);
    if (err != ESP_OK) {
        printf("link install failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"ok\":true,\"applet_id\":%u,\"bytes\":%u,\"replaced\":%s}\n",
               NEO_LINK_APPLET_ID, (unsigned)len, replace ? "true" : "false");
    } else {
        printf("Installed Hello World (0x%04X). Open it with Left Shift+Tab at power-on.\n",
               NEO_LINK_APPLET_ID);
    }
    return 0;
}

static int cmd_link_verify(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "link verify")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }

    (void)argc;
    (void)argv;
    if (!usb_host_neo_is_connected()) {
        printf("link verify: Neo not connected\n");
        return 1;
    }

    size_t bundled_len = 0;
    const uint8_t *bundled = neo_link_applet_blob(&bundled_len);
    if (!bundled || bundled_len == 0) {
        printf("link verify: no bundled applet\n");
        return 1;
    }

    size_t cap = bundled_len + 64;
    uint8_t *got = malloc(cap);
    if (!got) {
        printf("link verify: out of memory\n");
        return 1;
    }

    printf("Fetching applet 0x%04X from Neo (expect >= %u bytes)…\n", NEO_LINK_APPLET_ID,
           (unsigned)bundled_len);
    size_t out_len = 0;
    esp_err_t err = usb_host_neo_fetch_applet(NEO_LINK_APPLET_ID, got, cap, &out_len);
    if (err != ESP_OK) {
        free(got);
        printf("link verify: fetch failed: %s\n", esp_err_to_name(err));
        return 1;
    }

    printf("Neo returned %u bytes; bundled is %u bytes\n", (unsigned)out_len, (unsigned)bundled_len);
    if (out_len < bundled_len) {
        free(got);
        printf("link verify: FAIL — Neo blob shorter than bundled\n");
        return 1;
    }

    size_t mismatch = SIZE_MAX;
    for (size_t i = 0; i < bundled_len; i++) {
        if (got[i] != bundled[i]) {
            mismatch = i;
            break;
        }
    }
    free(got);

    if (mismatch != SIZE_MAX) {
        printf("link verify: FAIL — first byte mismatch at offset %u\n", (unsigned)mismatch);
        return 1;
    }

    if (out_len != bundled_len) {
        printf("link verify: OK — first %u bytes match (Neo padded to %u)\n", (unsigned)bundled_len,
               (unsigned)out_len);
    } else {
        printf("link verify: OK — exact match (%u bytes)\n", (unsigned)bundled_len);
    }
    return 0;
}
#endif /* CONFIG_BUDDY_NEO_LINK */

static int cmd_files_list(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "files list")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    (void)argc;
    (void)argv;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        printf("files list: out of memory\n");
        return 1;
    }
    if (file_manager_list(arr) != ESP_OK) {
        cJSON_Delete(arr);
        printf("files list: failed\n");
        return 1;
    }
    return uart_print_cjson(arr, "files list");
}

static int cmd_files_probe(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "files probe")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    (void)argc;
    (void)argv;
    char detail[160];
    esp_err_t err = file_manager_probe_backup_storage(detail, sizeof(detail));
    if (err != ESP_OK) {
        printf("files probe: FAIL %s\n", detail[0] ? detail : esp_err_to_name(err));
        return 1;
    }
    printf("files probe: OK %s\n", detail);
    printf("  flash_ready=%d free_bytes=%u base=%s\n", file_manager_flash_ready() ? 1 : 0,
           (unsigned)file_manager_free_bytes(), file_manager_base_path());
    return 0;
}

static int cmd_files_view(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "files view")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc < 1 || argv[0] == NULL) {
        uart_print_usage("files view");
        return 1;
    }
    char path[192];
    if (file_manager_resolve_path(argv[0], path, sizeof(path)) != ESP_OK) {
        printf("files view: invalid name\n");
        return 1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        printf("files view: open failed\n");
        return 1;
    }
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    fwrite(buf, 1, n, stdout);
    if (n == sizeof(buf) - 1) {
        printf("\n… (truncated)\n");
    } else if (n == 0 || buf[n - 1] != '\n') {
        printf("\n");
    }
    return 0;
}

static int cmd_files_delete(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "files delete")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc < 1 || argv[0] == NULL) {
        uart_print_usage("files delete");
        return 1;
    }
    esp_err_t err = file_manager_delete(argv[0]);
    if (err != ESP_OK) {
        printf("files delete: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"deleted\":true}\n");
    } else {
        printf("Deleted %s\n", argv[0]);
    }
    return 0;
}

static int cmd_files_rename(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "files rename")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    if (argc < 2 || argv[0] == NULL || argv[1] == NULL) {
        uart_print_usage("files rename");
        return 1;
    }
    esp_err_t err = file_manager_rename(argv[0], argv[1]);
    if (err != ESP_OK) {
        printf("files rename: %s\n", esp_err_to_name(err));
        return 1;
    }
    if (uart_want_json()) {
        printf("{\"renamed\":true}\n");
    } else {
        printf("Renamed %s -> %s\n", argv[0], argv[1]);
    }
    return 0;
}

static int cmd_keyboard_recent(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "keyboard recent")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    (void)argc;
    (void)argv;
    char buf[2048];
    unsigned long seq = 0;
    if (neo_live_snapshot(buf, sizeof(buf), &seq) != ESP_OK) {
        printf("keyboard recent: failed\n");
        return 1;
    }
    if (uart_want_json()) {
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            printf("keyboard recent: out of memory\n");
            return 1;
        }
        cJSON_AddStringToObject(root, "text", buf);
        cJSON_AddNumberToObject(root, "sequence", (double)seq);
        return uart_print_cjson(root, "keyboard recent");
    }
    printf("[%lu] %s\n", seq, buf);
    return 0;
}

static int cmd_keyboard_clear(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "keyboard clear")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    (void)argc;
    (void)argv;
    neo_live_clear();
    if (uart_want_json()) {
        printf("{\"cleared\":true}\n");
    } else {
        printf("Live typing buffer cleared\n");
    }
    return 0;
}

static int cmd_keyboard_raw(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "keyboard raw")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    (void)argc;
    (void)argv;
    char out[2048];
    if (hid_debug_get_json(out, sizeof(out), 20) != ESP_OK) {
        printf("keyboard raw: failed\n");
        return 1;
    }
    printf("%s\n", out);
    return 0;
}

static int cmd_keyboard_keylog(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "keyboard keylog")) {
        return 0;
    }
    if (!uart_require_auth(argc, argv)) {
        return 1;
    }
    const char *arg = (argc >= 1 && argv[0]) ? argv[0] : "status";
    if (strcasecmp(arg, "on") == 0 || strcasecmp(arg, "1") == 0 || strcasecmp(arg, "enable") == 0) {
        neo_live_set_key_log(true);
    } else if (strcasecmp(arg, "off") == 0 || strcasecmp(arg, "0") == 0 ||
               strcasecmp(arg, "disable") == 0) {
        neo_live_set_key_log(false);
    } else if (strcasecmp(arg, "status") != 0 && strcasecmp(arg, "show") != 0) {
        printf("keyboard keylog: use on|off|status\n");
        return 1;
    }
    const bool on = neo_live_get_key_log();
    if (uart_want_json()) {
        printf("{\"keylog\":%s}\n", on ? "true" : "false");
    } else {
        printf("Keystroke UART log: %s\n", on ? "on" : "off");
    }
    return 0;
}

static int cmd_ping(int argc, char **argv)
{
    if (uart_help_requested(argc, argv, "ping")) {
        return 0;
    }
    bool want_json = uart_strip_json_flag(&argc, argv);
    bool prev_json = s_cli_json;
    s_cli_json = want_json;

    /* Top-level: argv[0] is "ping"; host is argv[1]. */
    const char *host = "one.one.one.one";
    if (argc >= 2 && argv[1] != NULL && argv[1][0] != '\0') {
        host = argv[1];
    }

    int rc = 1;
    if (!wifi_manager_is_connected()) {
        printf("ping: not connected to Wi-Fi (join home network first)\n");
        goto done;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    int64_t t_dns0 = esp_timer_get_time();
    int gai = getaddrinfo(host, "443", &hints, &res);
    int64_t dns_us = esp_timer_get_time() - t_dns0;
    if (gai != 0 || res == NULL) {
        printf("ping: DNS resolve failed for %s\n", host);
        goto done;
    }

    char ipstr[INET_ADDRSTRLEN] = {0};
    struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        printf("ping: socket failed\n");
        goto done;
    }
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int64_t t_c0 = esp_timer_get_time();
    int cret = connect(sock, res->ai_addr, res->ai_addrlen);
    int64_t connect_us = esp_timer_get_time() - t_c0;
    close(sock);
    freeaddrinfo(res);

    if (cret != 0) {
        if (uart_want_json()) {
            printf("{\"ok\":false,\"host\":\"%s\",\"ip\":\"%s\",\"dns_ms\":%.1f,\"error\":\"connect\"}\n",
                   host, ipstr, (double)dns_us / 1000.0);
        } else {
            printf("ping %s (%s): DNS ok (%.1f ms), TCP 443 failed\n", host, ipstr,
                   (double)dns_us / 1000.0);
        }
        goto done;
    }

    if (uart_want_json()) {
        printf("{\"ok\":true,\"host\":\"%s\",\"ip\":\"%s\",\"dns_ms\":%.1f,\"connect_ms\":%.1f}\n", host,
               ipstr, (double)dns_us / 1000.0, (double)connect_us / 1000.0);
    } else {
        printf("ping %s (%s): ok — DNS %.1f ms, TCP 443 %.1f ms\n", host, ipstr,
               (double)dns_us / 1000.0, (double)connect_us / 1000.0);
    }
    rc = 0;

done:
    s_cli_json = prev_json;
    return rc;
}

static void uart_completion(const char *buf, linenoiseCompletions *lc)
{
    if (buf == NULL || lc == NULL) {
        return;
    }
    size_t blen = strlen(buf);
    const char *group = NULL;
    size_t group_cmd_len = 0;
    const char *partial = NULL;

    if (strncmp(buf, "neo ", 4) == 0) {
        group = "neo";
        group_cmd_len = 3;
        partial = buf + 4;
    } else if (strncmp(buf, "device ", 7) == 0) {
        group = "device";
        group_cmd_len = 6;
        partial = buf + 7;
    } else if (strncmp(buf, "files ", 6) == 0) {
        group = "files";
        group_cmd_len = 5;
        partial = buf + 6;
    } else if (strncmp(buf, "keyboard ", 9) == 0) {
        group = "keyboard";
        group_cmd_len = 8;
        partial = buf + 9;
    } else if (blen >= 3 && strncmp(buf, "neo", 3) == 0 && (blen == 3 || buf[3] == ' ')) {
        group = "neo";
        group_cmd_len = 3;
        partial = (blen > 4) ? buf + 4 : "";
    } else if (blen >= 6 && strncmp(buf, "device", 6) == 0 && (blen == 6 || buf[6] == ' ')) {
        group = "device";
        group_cmd_len = 6;
        partial = (blen > 7) ? buf + 7 : "";
    } else if (blen >= 5 && strncmp(buf, "files", 5) == 0 && (blen == 5 || buf[5] == ' ')) {
        group = "files";
        group_cmd_len = 5;
        partial = (blen > 6) ? buf + 6 : "";
    } else if (blen >= 8 && strncmp(buf, "keyboard", 8) == 0 && (blen == 8 || buf[8] == ' ')) {
        group = "keyboard";
        group_cmd_len = 8;
        partial = (blen > 9) ? buf + 9 : "";
    } else if (blen >= 4 && strncmp(buf, "link", 4) == 0 && (blen == 4 || buf[4] == ' ')) {
        group = "link";
        group_cmd_len = 4;
        partial = (blen > 5) ? buf + 5 : "";
    }

    if (group) {
        size_t partial_len = strlen(partial);
        for (size_t i = 0; i < s_command_count; ++i) {
            const char *cmd = s_commands[i].command;
            if (strncmp(cmd, group, group_cmd_len) != 0 || cmd[group_cmd_len] != ' ') {
                continue;
            }
            const char *suffix = cmd + group_cmd_len + 1;
            if (partial_len == 0 || strncmp(suffix, partial, partial_len) == 0) {
                linenoiseAddCompletion(lc, cmd);
            }
        }
        return;
    }

    esp_console_get_completion(buf, lc);
}

static void register_uart_commands(void)
{
    for (size_t i = 0; i < s_command_count; ++i) {
        const uart_cmd_entry_t *entry = &s_commands[i];
        /* esp_console rejects command names containing spaces. */
        if (strchr(entry->command, ' ') != NULL) {
            continue;
        }
        esp_console_cmd_t cmd = {
            .command = entry->command,
            .help = entry->summary,
            .hint = entry->hint,
            .func = entry->func,
        };
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmd));
    }
    /* Replace ESP-IDF's built-in help (registered during esp_console_init). */
    ESP_ERROR_CHECK(esp_console_cmd_register(&(esp_console_cmd_t){
        .command = "help",
        .help = "display help for commands",
        .hint = "[COMMAND]",
        .func = cmd_help,
    }));
    linenoiseSetCompletionCallback(&uart_completion);
}

esp_err_t uart_cmd_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "neo2> ";
    repl_config.max_cmdline_length = 512;
    repl_config.max_history_len = 64;
    repl_config.task_stack_size = UART_REPL_STACK_SIZE;

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_uart(&uart_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console REPL init failed: %s", esp_err_to_name(err));
        return err;
    }

    register_uart_commands();

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console REPL start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Full welcome banner is printed after startup_task finishes (see main.c). */
    ESP_LOGI(TAG, "UART command console started");
    return ESP_OK;
}
